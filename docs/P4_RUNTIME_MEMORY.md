# P4 runtime memory investigation — 2026-10-16

## Runtime graph

`tools/p4/ram_monitor.py` uses its adjacent HTML page and an existing
non-resetting serial logger's FIFO/log. It never opens USB, logs in, resets or
flashes. Run it while the existing logger is authenticated and running:

```sh
python3 tools/p4/ram_monitor.py --fifo PRIVATE_FIFO --log PRIVATE_LOG \
  --output PRIVATE_DIRECTORY/runtime.jsonl
```

Open `http://127.0.0.1:8766/`. Only localhost is served; no external chart service
is used. Only numeric memory counters and fixed event labels are published.
The monitor sends `memreport json` two seconds after each reply. Missing replies
time out after eight seconds and back off ten seconds. Missing samples break
the graph; brief allocation peaks may occur between samples. View pause does
not pause capture. CSV export includes the retained samples. Raw logs remain
private. Tests: `python3 tools/p4/test_ram_monitor.py`.

## Captured evidence

Previous camera-off app SHA-256:
`e3a445a05cceb58bdac5a4ef10a0ff77eda5f1f93ffeaf0a5820d7f7a41483c0`.
The internal heap total is 457,235 B; static code/data and cache are separate.
During the user's Hey Even attempt at 05:22 local time:

| Point | Internal free | Largest internal block | PSRAM free |
|---|---:|---:|---:|
| Before capture | 175,727 B | 98,304 B | 32,535,496 B |
| During capture | 149,503 B | 46,080 B | 26,893,284 B |
| Model loading, sampled | 153,587 B | 46,080 B | 6,781,152 B |
| HTTP activity after speech | 160,899 B | 98,304 B | 32,453,868 B |
| After completion | 175,727 B | 98,304 B | 32,534,208 B |

Warm-up and drafts failed; the final segment succeeded. The 20,112,128-byte
model loaded in 4,452 ms. Resident construction reported zero net internal
growth, no metadata fallback, and a 1,024,000-byte external activation arena.
These are net/resident measurements, not a bound on transient allocations.
The 150 KiB cold-admission policy remains unchanged.

An earlier Terminal speech failure ended with `no-complete-speech` and about
135,479 B internal free. A later status showed `host-request-failed-no-retry`.
Read-only JTAG with the installed ELF confirmed the retained result: Wi-Fi,
HTTP status 0, no body, `Terminal connection failed; no retry`. This does not
distinguish TCP, TLS or allocation failure. Terminal's HTTP worker uses an
8 KiB internal stack plus network/TLS allocations and may overlap speech.
That is a candidate explanation for additional pressure, not a proven cause
of the connection failure. Do not conflate the two closes. The subsequent chat
status was READY with no error; that alone does not establish reply delivery.

## Changes flashed with approval

- `Gatt` borrows a static-lifetime `Session`. Its 19,604-byte protocol storage
  goes into static PSRAM when external BSS is supported, otherwise internal
  memory. Explicit placement construction preserves nonzero defaults. Callback
  queue, locks, secure-channel state and required internal `PhoneTls` stay
  internal. No protocol change; storage survives quarantine and host recovery.
- Native Terminal speech failure discards the incomplete draft, displays the
  recognizer error and keeps the host view. It waits for older ACKs before
  resetting speech UI, never submits partial speech, and rejects late confirms.
- Optional speaker identification is skipped after primary warm-up fails.
  Warm-up errors and admission counters are logged without the debug toggle.
- Task accounting matches the containing live stack allocation: P4 advances
  `pxStackBase` after reserving coprocessor state inside a stack. Exact-start
  matching missed those stacks. Unknown allocations are labelled unmeasured.

| ELF section | Installed | Candidate |
|---|---:|---:|
| Internal `.data` | 57,916 B | 29,372 B |
| Internal `.bss` | 68,672 B | 77,600 B |
| External `.bss` | 594,556 B | 614,156 B |

Net static internal reduction: **19,616 B (19.16 KiB)**. The approved P4-only
flash verified successfully on 2026-10-16. Live internal heap capacity increased
from 457,235 B to 476,851 B, exactly 19,616 B. After both glasses links connected,
a sample showed 197,491 B internal free (largest block 159,744 B). Idle free memory depends on active services and is not a controlled
before/after comparison. The wearer subsequently confirmed a Terminal message
was sent; the detailed observation below records the limits of that test.

## Verification and hardware gate

Passing host suites: native Terminal (sanitizers and disabled variants),
continuous STT, task heap matching, phone GATT, phone runtime (internal/external
construction), phone recovery, SDK callback lifetime, ESP-DL metadata and RAM
monitor parsing. New task-heap and monitor tests are registered with CTest.

| Successful profile | App bytes | Partition headroom |
|---|---:|---:|
| p4-phone-pipe | 5,933,488 | 1,492,560 |
| p4-openclaw | 5,900,800 | 1,525,248 |
| p4-phone-pipe-camera-off-test | 5,620,944 | 1,805,104 |

Camera-off candidate SHA-256:
`1947d9950f91632b1648abf41675d9451789649f83a725be3da1464eb3d840aa`.
User approved and the P4 app alone was flashed at `0x10000`; esptool verified
the written data hash. C6 firmware was unchanged, and `c6status` still reports
three BLE links (assumed because the companion does not report its limit).
USB commands and runtime RAM sampling respond; both glasses connected.
Compare recording RAM and retry Terminal speech and HTTP separately. TLS verification stays unchanged. Camera fault remains separate.
No commits, phone, ring, relay or firewall changes.

Private evidence and compressed installed app/ELF/map remain in the existing
ignored live-test `memory-audit/` directory for rollback and exact-symbol
debugging until the replacement is qualified. Existing build directories were
reused; no full source/dependency snapshots were created.

## First successful Terminal speech/send after flash

At 05:53 local time the wearer confirmed that recording and sending worked.
Primary model warm-up and final transcription both reported `ok=1`; audio
overruns were zero. Terminal remained owned and reported `receiving` afterward.
The sampled internal free minimum over 05:52:55–05:53:30 was 151,791 B;
the smallest sampled internal block was 58,368 B, and sampled PSRAM free
reached 3,208,800 B. The boot-wide internal low-water counter was 58,616 B;
this is not the same as the two-second sampled minimum. Full response delivery
was not separately confirmed. A connection-error line preceded the speech
capture, so this single success does not qualify every network request.

The first opening attempt had failed before host admission because glasses
firmware metadata was absent. Its one-shot BASIC query has a 1.5 s timeout;
local teardown left the glasses showing “connecting to host.” A manual BASIC
refresh populated both versions as `2.3.2.14`, and reopening worked. This
startup retry/display issue remains unresolved; the SRAM flash did not fix it.

## Long-recording poll failure and prepared fix

The next, longer recording warmed the speech model successfully, but the
HTTP client logged a failed connection at 05:53:55. Terminal subsequently
reported `host-request-failed-no-retry`. The production adapter permits polls
during recording and processed that error by closing the view and cancelling
speech. A temporary production-owner regression reproduced this independently:
a failed read-only poll cancelled speech with zero prompts submitted. The
recording also had a failed draft before the connection failure; its underlying
recognition error was not separately resolved by this investigation.

Over 05:53:40–05:54:00, sampled internal free reached 149,731 B, the smallest
sampled internal block was 54,272 B, and sampled PSRAM free reached 3,167,760 B.
The boot-wide internal low-water counter was 50,648 B. These measurements do not
identify the underlying connection error as a memory failure.

The prepared fix handles failed read-only polls independently of mutations.
Transport/status-zero and server failures retry three times after 3, 6 and
12 seconds from completion, respecting stream admission and retirement. Then
polling pauses without cancelling speech, discarding the draft or closing the
view. Non-transient HTTP failures pause immediately; relay 503 keeps its existing
no-retry/outcome-unknown handling. Normal polling cadence is unchanged. An
explicit operation or reopening can resume polling. Confirmed mutating requests
retain their no-automatic-replay behavior.

Direct-connection diagnostics report stage, failure category (network, trust,
cancelled, protocol, resources), raw TLS/verification codes, socket error, elapsed
time/deadline and free/largest internal memory plus PSRAM. Logged categories are
numeric in that order, starting at zero; stages and numeric counters contain no
hostnames, addresses, credentials or request text. The connection deadline and
TLS validation policy remain unchanged.

Host verification passes with ASan/UBSan: native Terminal (all four feature
variants), production HTTP transport, pinned IDF open/header/body integration,
and HTTP worker lifecycle. Tests cover active speech/draft preservation on
both paths, recovery, retry exhaustion, confirmation after exhaustion, 401/404/503
pause behavior, no automatic mutation replay, and diagnostic contents.

The currently installed RAM-fix app, ELF and map were compressed into the
private `memory-audit/installed-ram-fix-reference/` before reusing its build
directory. Retain them until the poll fix is qualified. No source/dependency
snapshot was created. The user approved the poll fix, and the P4 app-only flash at `0x10000`
completed with a verified data hash. C6 remains unchanged at its assumed
three-link limit. USB responds, both glasses links connected, and a post-boot
sample shows 197,955 B internal free with a 159,744 B largest block.
The wearer subsequently confirmed longer messages worked, but another recording
displayed the internal-SRAM admission warning investigated below. This is partial
hardware qualification, not a pass for repeated long recordings.

All three poll-fix builds passed: `p4-phone-pipe` 5,934,832 B;
`p4-openclaw` 5,901,600 B; camera-off test 5,622,304 B with
1,803,744 B app headroom. Camera-off SHA-256:
`e4224c061a9ade47e2afb25fdd893c3649c9985538aa9341d0377644b37bd30a`. Flash target is P4 app only at `0x10000`.

## Speech admission and allocation overlap follow-up

At 06:12 local time, the primary model warmed successfully. Resident construction
reported 148 B net internal growth, a 1,024,000 B PSRAM activation arena and no
metadata fallback. Later calls were refused with **149,747 B internal free**
against a **153,452 B** admission floor, despite **4,997,280 B PSRAM free**
against a 421,896 B call budget. The warning was a policy refusal, not an observed
failed SRAM allocation. The language model was unnecessarily released first.

The former resident floor subtracted whole-system free-heap change during model
construction from the 150 KiB cold-construction requirement. That delta includes
HTTP, Bluetooth and other task activity; it cannot measure model ownership.
Metadata placement improvements also made this credit nearly zero, so a loaded
model effectively faced the cold-construction floor on every phrase.

The follow-up uses a separate **128 KiB resident admission reserve**, while cold
construction still requires **150 KiB**. This is a conservative policy choice to
qualify on hardware, **not a measured upper bound on kernel scratch**. It does
not allocate or reserve a new buffer. Optional language-model memory is released
only for PSRAM pressure/contiguity, never for an internal-only refusal. The
existing net `int_used` diagnostic remains a global delta, not attribution.

Native Terminal stops starting background polls while Recording or Finishing.
An existing HTTP request may finish normally; local glasses heartbeats continue.
Polling resumes after transcription finishes, including while waiting for user
confirmation. The Wi-Fi 500 ms and phone completion-based 2/3 s cadences remain
unchanged outside those phases. This explicitly delays host transcript/query
updates while recording; it prevents repeated HTTP worker and connection
allocations from overlapping inference. It does not cancel or replay a mutation.

### What uses memory during a spoken Terminal request

The path is G2 microphone → BLE reception/LC3 decode → PCM/VAD and feature
extraction → speech model and text decoder → native Terminal draft → confirmed
HTTP request → Wi-Fi/SDIO/C6 or phone pipe. Bluetooth transports both microphone
audio and glasses UI traffic through the same P4 host; there are not two copies
of the Bluetooth stack. The C6 has its own RAM, unavailable to the P4 allocator.

| Consumer | Internal SRAM behavior | PSRAM behavior / lifetime |
|---|---|---|
| Persistent system tasks, BLE host, SDIO, network/event tasks, display and command services | Baseline stacks, TCBs, queues and driver state; packet traffic adds transient allocations | Application queues and many buffers already use PSRAM |
| Speech worker, audio capture and feature worker | 12,288 + 8,192 + 3,072 = **23,552 B stacks**, plus TCBs/objects; created per recording | PCM, segments and cached features are predominantly external; released at session end |
| QuartzNet inference | Kernel scratch and some small SDK allocations vary per layer; cold construction has additional transients | **20,112,128 B weights**, **1,024,000 B resident arena**, metadata; retained across phrases within the recording |
| Text decoding | Some small objects/scratch; do not infer ownership from a global heap delta | Optional language model approximately **1.6 MB**; now retained on internal-only pressure |
| Optional speaker identification | Small control/TCB costs and runtime scratch; net load delta is not a peak bound | Approximately **3.3 MB** including model/workspace; still loads after successful primary warm-up, even though Terminal does not display labels |
| Terminal HTTP worker and connection | **8,192 B worker stack per request**, plus TCB, socket/TCP and wrapper allocations | Request/result storage and configured mbedTLS allocations are external; released on completion |
| Phone pipe | Task, callback/security state and required TLS state remain internal | Protocol session storage moved to static PSRAM in the preceding fix |

The configured default-malloc threshold is 16 KiB: small allocations without
explicit placement prefer internal memory. Moving the large model or request
buffer to PSRAM therefore does not move every C++ container, SDK scratch buffer,
task stack or network object. mbedTLS is configured for external allocations
(16 KiB input / 4 KiB output records); attributing all TLS buffers to SRAM would
be incorrect. Speech's optional keep-warm mode is unchanged. Without it, successive
recordings still release and reload the model; this fix removes redundant churn
within a recording and avoids poll overlap rather than claiming a flat heap.

The 06:20 idle sample had **196,195 B internal free**, largest block **124,928 B**.
Its cumulative Terminal allocation counter covered 784 requests; cumulative
traffic is not live usage or proof of a leak. Two-second graph samples miss brief
peaks, and the boot-wide low-water mark is not a per-recording measurement.

### Correct task attribution before comparing consumers

The previous containment fix exposed a second accounting bug: IDF registers a
reserved 32 KiB internal/DMA pool inside a live allocation in its parent heap.
Depending on walk order, that parent overwrote the smaller child stack allocation,
making both the 4 KiB debug task and 8 KiB command task appear to have 32 KiB stacks.
The walker now selects the innermost heap, including free child blocks so a deleted
stack is not attributed to its live parent reservation. This changes attribution,
not physical free SRAM. The existing `hw1InternalTotalBytes()` helper already
subtracts the reserved pool from IDF's total to avoid double-counting capacity;
that total and the free-memory graph do not need this attribution correction.

### Verification and remaining measurement

Host regressions exercise the observed refusal values, the resident/cold boundary,
unrelated allocations and frees during model construction, preserving the LM on
internal-only pressure, and nested/free stack allocations in either traversal
order. Native Terminal tests cover deferred/queued polls during capture and drain,
continued heartbeats, completion and recovery, both transports, and disabled builds.
The host model doubles do not measure ESP-DL's actual allocation peaks.

The installed poll-fix app/ELF/map are preserved compressed (28,862,048 B) under
the private `memory-audit/installed-poll-fix-reference/` before reusing its build
directory. Retain this exact rollback/debug set until the replacement is qualified.
Existing profile build directories are reused; no source/dependency copy is made.
This follow-up needs a fresh P4-only flash approval. Qualification must compare
repeated short/long recordings, successful final text and send, no automatic
mutation retry, internal free/largest block and recovery to the idle baseline.
Optional speaker work and remaining SDK kernel scratch are the next candidates
for measured pooling or per-consumer opt-out; neither is silently disabled here.

All follow-up host suites passed with ASan/UBSan: QuartzNet cache (resident and
per-call, diagnostics on/off), native Terminal (enabled and three disabled
variants), continuous STT broker and task-heap attribution. All three builds
passed:

| Profile | App bytes | App headroom |
|---|---:|---:|
| p4-phone-pipe | 5,934,480 | 1,491,568 |
| p4-openclaw | 5,901,248 | 1,524,800 |
| p4-phone-pipe-camera-off-test | 5,621,952 | 1,804,096 |

Camera-off SHA-256:
`e3f61f8524c8dda74a923c2bc46aae8994adf27680102c6c0890e57087fbb22f`.
Its internal `.data` / `.bss` remain 29,372 / 77,600 B, unchanged from the
installed poll fix. The diagnostics add four bytes per measured task to the
PSRAM-preferred snapshot; there is no new permanent SRAM buffer. This candidate
has **not been flashed**. Raw evidence, test/build logs, source hashes and a
tracked working-tree patch are private; temporary test logs were removed.

## Additional PSRAM pass — 2026-10-16

This supersedes the unflashed admission-only candidate above; the P4 was subsequently flashed with this candidate after explicit approval
(see the runtime check below). It includes the admission/poll-overlap
changes plus the following memory-placement work.

| Storage moved | Bytes | Lifetime |
|---|---:|---|
| Speech text queue | 4,352 | Static, existing bounded transcript retention and wiping |
| Speech performance ring | 2,752 | Static; nonzero sentinel defaults explicitly constructed |
| Audio-level diagnostic ring | 768 | Static, capture/worker task access |
| Stage accumulation and two report buffers | 3,072 | Static, inference/command task access |
| Total external storage | **10,944** | No new per-recording allocation |

Locks, atomics, session control, ISR/DMA state and task stacks remain internal.
Targets without external BSS retain their ordinary storage. The first profile's
ELF confirms **10,936 B net internal static saving**; a reference and per-session
control account for the 8 B difference. Free internal heap will be checked after
an approved flash, separately from this static layout result.

Native Terminal now passes a session-local speaker-label opt-out to the shared
STT broker. It still receives drafts/final text, but skips the optional speaker
model's load, submission, polling and drain. Based on the prior device load,
this avoids roughly **3.3 MB PSRAM** during Terminal speech, plus the optional
worker's activity. The exact runtime delta is pending measurement. Other native
consumers, including Conversate and QuickNote, keep the existing default. A new
session cannot inherit Terminal's opt-out accidentally.

### Inference setup reuse

On P4 the generic C convolution accumulator-buffer path is compiled out in favor
of PIE kernels; changing those generic allocations would not fix this device.
The active setup path did allocate per-channel factors with the default heap
and then copy them into each Conv's already-retained PSRAM `Prepared` block.
Setup now borrows that aligned block directly. Two split-core argument sets
retain their correct pointer offsets, and setup/run completion precedes reuse.
The block grows transactionally and remains owned by the Conv until destruction.
No global pool, extra permanent SRAM reserve or change to kernel arithmetic is
introduced. Temporary argument vectors and non-borrowed factors use the existing
metadata allocator (PSRAM preferred, counted internal fallback if necessary).

The host test repeats setup 100 times per quantization variant without another
factor/prepared-block allocation after the first use. It also covers owned
fallback, insufficient/unaligned borrow, split-pointer offsets, independent
layers, growth failure, and complete release. This measures that allocation
class only: temporary argument vectors and other SDK setup still allocate.
The ESP-DL patch and provenance hashes were updated and verified by a fresh
application to the pinned pristine package. Temporary validation copies were
removed. Numerical kernels and their execution order are unchanged; physical
recognition accuracy and latency still require the P4 test.

Host validation includes speaker-enabled continuous STT, native Terminal and
QuickNote with disabled variants, one-shot STT, QuartzNet's four cache variants,
metadata allocation/fallback tests, and the new sanitizer-enabled
`stt_conv_scratch` CTest registration. Builds and candidate identity follow below.
No flash, commit, C6, Android, relay or Terminal-host change was made by this pass.

All seven listed host suites passed, including sanitizer checks and disabled
variants. The final ELF verifies the same 10,936 B net saving in all profiles;
the startup object also contains the explicit nonzero sentinel initialization.

| Profile | App bytes | App headroom |
|---|---:|---:|
| p4-phone-pipe | 5,932,160 | 1,493,888 |
| p4-openclaw | 5,898,928 | 1,527,120 |
| p4-phone-pipe-camera-off-test | 5,619,632 | 1,806,416 |

Camera-off candidate SHA-256:
`571767002e92220637da3781141375059632bd3266b0f26ac44de4f1ec225ab8`.
Its image checksum/hash validate. Internal `.data` / `.bss` are
26,620 / 69,416 B; external `.bss` is 625,108 B. It contains both this pass and
the preceding unflashed admission/poll-overlap fix. The user approved this image
and the P4 app-only flash at `0x10000` completed with esptool hash verification;
the existing three-link C6 firmware was unchanged.

Existing profile build directories were reused (approximately 1.1 / 1.1 / 1.0
GiB), retained for review, flashing and incremental verification. No additional
build or source/dependency snapshot was retained. Temporary SDK validation trees,
header backups and temporary test logs were removed; selected logs, source hashes
and the tracked working-tree patch remain private. The already-preserved installed
poll-fix app was re-hashed successfully and remains the rollback image until the
new candidate passes repeated short/long speech, text/send, latency and RAM checks.

### Approved PSRAM-pass boot check, 2026-10-16

The camera-off image above booted, authenticated over USB, rejoined Wi-Fi and
connected both glasses temples. `c6status` still reports Hosted 2.12.13 and three
BLE links (assumed because this companion does not report its limit).

Measured internal allocator capacity increased from **476,851 to 487,779 B**,
a **10,928 B** gain. This is 8 B below the 10,936 B static-layout saving; report
the measured capacity separately rather than equating the two. After the glasses
connected, one sample showed **208,671 B internal free**, **172,032 B largest
internal block**, and **32,610,968 B PSRAM free**, before speech/model loading.
The RAM recorder resumed across the flash gap. Repeated short/long Terminal
speech, recognition/send behavior and loaded-model memory remain pending.

### Initial speech qualification after approved flash

Four wearer-triggered Terminal recording sessions produced **eight successful
final segments**, with no failed final segment; the longest recording produced
four segments. The user confirmed that it was working well. Each model setup
reported a PSRAM arena of 1,024,000 B, no internal arena or parameter-object
allocation, and zero metadata fallback/failure. No optional speaker-model load
appeared. This validates the Terminal opt-out but is not an isolated measurement
of its exact PSRAM delta. Minimum reported PSRAM during these sessions was
6,019,688 B. Internal minimum since boot reached **82,980 B**; the recorder's
periodic samples do not capture every brief allocation peak.

Remaining observations: the primary model reloaded between recordings, taking
about 4.5 seconds for read/inflate/hash, and internal SRAM still dips transiently.
Audio integrity warnings reported a few lost frames while final transcription
continued. The longest recording also logged `esp_task_wdt_reset: task not found`
warnings; no watchdog reset or final-transcription failure was observed. These
remain separate follow-ups, not proof that all allocation churn or audio/worker
lifecycle issues are resolved. Raw logs and numeric RAM capture remain private.
No further firmware change, flash, commit, C6 or relay change followed this test.

### Remaining-review source changes, 2026-10-16 (not yet flashed)

Speech admission now checks both total internal free bytes and the largest
contiguous internal block, at initial admission and again before constructing
the model. A bounded task-scoped allocation ledger measures live requested
internal bytes and the largest allocation during each successful transcribe
call. Concurrent HTTP/Bluetooth tasks, interrupt allocations and PSRAM allocations
are excluded. Measurements are separated for cold construction and resident
calls. Admission uses the larger of the existing conservative floor and the
observed owned peak plus 32 KiB; its contiguous requirement is at least 32 KiB,
or the observed largest allocation plus 1 KiB. No whole-system heap delta is
used. Existing worker stacks are already allocated when this model admission
runs; these measurements are not a claim about the complete subsystem's RAM.

The ledger has 128 slots (1,024 B on P4), no heap allocation and no locks inside
heap hooks. Overflow or an unsuccessful call cannot update admission. Same-pointer
realloc updates its entry; ESP-IDF does not identify the old pointer for moved
realloc, so a retained old entry can overestimate lifetime. Cross-task frees
can also overestimate lifetime. These are conservative bounds, not an exact
whole-program allocation profiler. Floors are not lowered from these samples.
The P4 speech profiles opt into `CONFIG_HEAP_USE_HOOKS`; global allocator
placement, heap task tracking and heap header layout are unchanged.

Inference no longer unregisters/re-registers idle watchdog subscriptions. Its
existing 250 ms stage-boundary blocking yields remain, with both idle watchdogs
configured throughout. This removes the subscription race; long-speech latency
and watchdog behavior still need validation on the new image.

The pipe follow-up accepts SDK NO_RSP 0x8f completion without replay and does
not treat handshake transport failures as failed authentication. Connection
failure diagnostics now label timeout/network/TLS-trust/protocol/resources and
retain numeric TLS/socket errors and internal total/largest free snapshots.
Socket ENOMEM/ENOBUFS is a resource failure and cannot cause network fallback.
No evidence yet ties the earlier background poll failure to SRAM exhaustion.

The approved working PSRAM-pass app and matching ELF/map were preserved as a
verified compressed recovery set (29,051,611 B) before reusing the camera-off
build. The existing three build directories are reused; no full SDK or source
snapshot was added. Firmware patches remain uncommitted.

### Remaining-review build and test results, 2026-10-16

All three builds passed, with heap hooks enabled only by their saved P4
profiles. The camera-off candidate resolves `ENABLE_CAMERA_SENSOR=0`.

| Profile | App bytes | Partition headroom | SHA-256 |
|---|---:|---:|---|
| `p4-phone-pipe` | 5,932,704 | 1,493,344 | `c6d7c4b5e8f2b8f96ad2e40fc23bc089eb51631a1b46a368ed2e8414e2f57c70` |
| `p4-openclaw` | 5,899,264 | 1,526,784 | `7953ba5ed033a59618a54f71893ac44cbeda94bb3bd5b8fae5decc8ea65fa3ad` |
| `p4-phone-pipe-camera-off-test` | 5,620,176 | 1,805,872 | `ad56d97d1ae6d449829039b41fc762eaf252742f2f0788ef146b6b9fa14e2d23` |

Relative to the currently installed PSRAM-pass camera-off image, linked internal
BSS grows by 1,048 B and aligned IRAM by 384 B. Including alignment, `_heap_start`
moves forward **1,424 B**. This is about 1.4 KiB of internal capacity, not a
runtime peak measurement. External BSS is unchanged at 625,108 B. Both heap hooks
and their direct callees are in IRAM; ledger operations are inlined.

Verification: 22/22 phone CTests, 6/6 speech/notes/RAM CTests, and 4/4 final
factory-reset/QuickNote/memory checks passed (the memory test overlaps the group
of six). QuartzNet cache tests passed all four resident/diagnostics combinations
with sanitizers. The latest allocation-scope test passed with hooks enabled and
disabled, including rejection of a second task's overlapping scope. Diff
whitespace checks passed. Existing compiler warnings remain; builds are not
claimed warning-free.

This candidate is **not flashed**. After explicit P4-only approval, repeat short
and long Terminal recordings, verify the view survives a refused admission,
and inspect `STT: internal owned` peak/largest/complete values alongside RAM and
watchdog logs. A complete ledger means no slot overflow, not perfect accounting
of allocations made by other tasks. Keep the conservative floors until measured
hardware results justify a separately reviewed adjustment.

Android interoperability, exactly-once host correlation, connected-ring yielding
and four-link C6 qualification remain physical test gates. D5 and the camera
fault remain outside this pass. Changes are grouped for review but are not split
into commits: the no-commit/no-staging instruction is still in effect.

### Phone-pipe stage-4 candidate flashed, 2026-10-16 evening

The user approved this image. It is the `p4-phone-pipe-camera-off-test` build
(G2 and ring compiled in, camera off). It contains the remaining-review changes
above plus the phone-pipe firmware rounds 4 and 5:

- **Round 4:** F1 auto-reconnect on enrollment, F2 glasses-off admission, and
  the recycle guard.
- **Round 5:** F5 two-way path fallback, A-1 OPEN phases, F3 path records and
  `phonepipe stats`, the A-3 phone reconnect cap, A-4 per-connection parameters
  and A-5 expiry attribution.

See `exchange/firmware-003.md` and `firmware-004.md` in the bridge project's
`.local/android-ble-relay/`.

| Item | Value |
|---|---|
| App image SHA-256 | `b22762943b2ea760ec9df846912d266b9c910aa18df7b9187a8a6a5acabca891` |
| Size / headroom | 5,625,472 B / 24.2% of the 7,252 KB factory slot |
| ELF SHA-256 prefix | `e7b1ae228` (printed at boot) |
| Method | esptool 4.12.0 `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks before writing:**
- The chip identified as ESP32-P4 rev 3.2.
- The device's partition table was byte-identical to the build's.
- Secure boot and flash encryption are disabled (`SPI_BOOT_CRYPT_CNT` 0).

**Checks after writing:**
- esptool hash verification passed.
- A full readback of the app region was byte-identical to the image.
- NVS, the LittleFS data partition and the C6 companion were not touched; the C6
  still reports the three-link build.

**Boot check (120 s capture, no JTAG):**
- There was one P4 boot, caused by the USB reset that started the capture, and no
  panic, watchdog or further reset.
- Saved Wi-Fi rejoined, the G2 client and the BLE peer auto-reconnect started,
  and the HTTP server came up. Setup completed at 14.7 s.
- The `phone_pipe` task is running, with no "Phone pipe unavailable" message.
- The serial console answered `help` probes at 25, 60 and 100 s with its login
  gate.
- Two Arduino BLE wrapper errors (`parseAdvertisement` 0-byte allocation, and
  `retrieveDescriptors` Unknown) also appear in the 2026-10-15 stage-3 console log.

Evidence (private, ignored) is in `tools/p4/companion/private/flash-2026-10-16/`:
the boot log, the esptool logs, the device partition table and the build manifest.

**Soak (approved, same evening):**
- Setup: a fresh boot from a USB reset, a 780 s serial capture, no JTAG, and a
  `help` probe every 30 s.
- Result: all 25 probes (30–750 s) were answered at the login gate. There was
  one P4 boot and no further reset, panic, watchdog or serial error. Device
  uptime reached 776 s, past the roughly 613 s point where the camera-enabled
  image used to hit `HP_SYS_HP_WDT_RESET`.
- Repeating log lines: Arduino BLE wrapper messages `parseAdvertisement` 0-byte
  allocation (17), `BTM_BleScan scan not active` (13) and `retrieveDescriptors`
  Unknown (3). All three also appear in the 2026-10-15 stage-3 log.
- This is evidence for the camera-off image only. It does not fix the camera
  fault, and it does not qualify the phone pipe.

Not yet done:
- Authenticated `phonepipe status` / `stats` (needs a login).
- Android stage-4 interoperability and the FW-6 switching rows (the pipetest APK
  is built but not installed).

### Phone-pipe reconnect fix flashed, 2026-10-16 late evening

The user approved this image. It is the same `p4-phone-pipe-camera-off-test`
build plus one firmware change: `bleRegisterPeer` now replaces the boot-time
metadata-only phone entry in the ordered peer list when the pipe registers
its connectable spec. Before the fix, boot reconnect, the reconnect tick and
`bleautoreconnect phone` never reached the phone (android-006 Note 1). See
`exchange/firmware-006.md`.

| Item | Value |
|---|---|
| App image SHA-256 | `cea8333b2a6b0ae279dd2c98a53a81c72803c606aaa1c4639aafcc5bab441b4e` |
| Size / headroom | 5,625,504 B / 24% of the 7,252 KB factory slot |
| ELF SHA-256 prefix | `dd4f0c6d6` (printed at boot) |
| Method | esptool `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks before writing:** ESP32-P4 rev 3.2. The device partition table was
byte-identical to the build's. Secure boot and flash encryption are disabled
(`SPI_BOOT_CRYPT_CNT` 0).

**Checks after writing:** esptool hash verification passed, and a full
readback of the app region was byte-identical to the image. NVS, the LittleFS
data partition and the C6 companion were not touched, so the pipe passphrase
set earlier on the old image is kept. The C6 still reports network_adapter
2.12.13 (the three-link build).

**Boot check (120 s capture, no JTAG):**
- One P4 boot, caused by the USB reset that started the capture. No panic,
  watchdog or further reset.
- Wi-Fi rejoined, the G2 client started, the boot reconnect pass ran, and the
  HTTP server came up. Setup completed at 14.0 s.
- The `phone_pipe` task is running, with no "Phone pipe unavailable" message.
- The console answered `help` probes at 25, 60 and 100 s with its login gate.

Not yet done: the logged-in `bleautoreconnect phone` check (it needs the
user's login), and the android-006 checklist on this image.

### Phone-pipe scan and coexistence fix flashed (firmware-007), 2026-10-17

The user approved this image: the `p4-phone-pipe-camera-off-test` build with
firmware-007:
- **The reconnect scan:** continuous 60/60 ms, no duplicate filter, up to 4 s,
  stopping at the first match.
- **Per-scan `PIPE_SCAN` records and a `PIPE_SETUP` breakdown.**
- **Scan fixes:** the F1–F3 scan-ownership and stop fixes.
- **MTU:** the resend and BUSY retry were removed.
- **End reasons:** each refused frame keeps the core's cause.

| Item | Value |
|---|---|
| App image SHA-256 | `4161de4ecf5bf3db1606446bf927e4da985f830ddb805237da335780a0967e3f` |
| Size / headroom | 5,630,368 B / 24.2% |
| ELF SHA-256 prefix | `92b6dd232` (printed at boot) |
| Replaced | `84bdbf8a…30a8` (ELF `aa8a39f3…`), read back from the device before writing and kept as `rollback/image-84bdbf8a.bin` |
| Method | esptool `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks before writing:** ESP32-P4 rev 3.2. The partition table was identical
to the build's. Secure boot and flash encryption are off.

**Checks after writing:** esptool hash verification passed, and the full
readback was byte-identical. NVS, LittleFS and the C6 were not touched, so the
phone enrollment and passphrase are kept.

**Boot check (120 s, no JTAG).** One P4 boot, no panic or watchdog; the console
answered all three `help` probes at its login gate.

**The phone pipe at boot, with Wi-Fi associated:**
- **Scan #1** found the phone 201 ms after it started (15 reports, 1 match).
- **The first link attempt** failed with HCI 0x3e (connection failed to be
  established) while the G2 glasses were reconnecting after boot.
- **Scan #2,** once the glasses had connected, found the phone in 344 ms.
- **Setup** to `ready` took 7,523 ms: connect 1,151, Bluedroid automatic
  discovery 5,700, MTU 100, secure handshake (including the CCCD write) 392,
  pipe HELLO 180.

The G2's stray "BTM_BleScan scan not active" stop still logs; the pipe now
ignores it.

Evidence is in `tools/p4/companion/private/flash-2026-10-17/`.

### BLE coexistence records flashed (lever 1 and phone attempts), 2026-10-17 evening

The user approved this image: the `p4-phone-pipe-camera-off-test` build with
lever 1 of `docs/WIFI_BLE_COEXISTENCE_STUDY.md` and the per-attempt phone
record that answers Android round 10 (`android-009.md`):
- **Records:** per-scan and per-connect records for G2, the ring and the phone,
  each with the Wi-Fi state, flags, last disconnect and Wi-Fi activity around
  it (`blerecords`; `ENABLE_BLE_COEX_RECORDS` on).
- **Phone attempts:** one record per scan that found the phone, with the step
  reached, the first non-OK GATT status, the phone's own disconnect reason, the
  cause and its source for cancels, and the wait for the setup gate.
- **Status text:** the four cancels that shared "phone operation cancelled"
  now name their source.
- **No radio behaviour change.**

| Item | Value |
|---|---|
| App image SHA-256 | `fe171599b2f40e4cf118a39aa81243b5cff5bdc8d0d47c11059d74dba10d6f33` |
| Size / headroom | 5,642,736 B / 24.0% |
| ELF SHA-256 prefix | `3ccabc318` (printed at boot) |
| Replaced | `4161de4e…7e3f`, read back from the device before writing and kept as `rollback/image-4161de4e.bin` |
| Method | esptool `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks before writing:** ESP32-P4 rev 3.2. The partition table was identical
to the build's. Secure boot and flash encryption are off.

**Checks after writing:** esptool hash verification passed, and the full
readback was byte-identical. NVS, LittleFS and the C6 were not touched, so the
phone enrollment and passphrase are kept.

**Boot check (120 s, no JTAG).** One P4 boot (the capture's USB reset; the
other ROM banners are the C6 companion's), no panic or watchdog. Setup
completed, the `phone_pipe` task ran, and the console answered all three
`help` probes at its login gate.

**The first records, with Wi-Fi associated throughout:**
- **Phone scan #1** found the phone in 219 ms. **Attempt #1** was cancelled at
  `stage=discovery` with `cause=14(yielded)`: the glasses' saved reconnect took
  the setup gate. This is the round-10 yield, seen live.
- **G2 scan #2** found both temples (saved); the left and right temples
  connected in 1,918 and 1,509 ms.
- **Phone scan #3** found the phone in 83 ms; **attempt #4** reached `ready` in
  7,163 ms after waiting 1,103 ms for the setup gate (discovery 5,670 ms).

Evidence is in `tools/p4/companion/private/flash-2026-10-17b/`.

### Menu status dot flashed, 2026-10-18

The user approved this image: the `p4-phone-pipe-camera-off-test` build with
the BLE coexistence records still on, and the menu's right-pane decorator
redrawn. The availability badge (`D` or `X`) and a status dot (filled when the
item is ready, hollow otherwise) now stack in one column between the divider
and the icon, each 2 px from the divider, the header and each other, so the
icon no longer moves and nothing needs the bottom row.

| Item | Value |
|---|---|
| App image SHA-256 | `0ed5ad6173f9b66503945bcc0dd3d93eb4c5ef25436099929e19d25594dd5ae9` |
| Size / headroom | 5,642,496 B / 24.0% |
| ELF SHA-256 prefix | `e82cc11f2` (printed at boot) |
| Replaced | `f9cbb28e…0d3f` (5,910,272 B), read back from the device before writing and kept as `rollback/image-f9cbb28e.bin` |
| Method | esptool `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks before writing:** the partition table was identical to the build's.
Secure boot and flash encryption are off.

**Checks after writing:** esptool hash verification passed, and the full
readback was byte-identical. NVS, LittleFS and the C6 were not touched, so the
phone enrollment and passphrase are kept.

**Boot check (120 s, no JTAG).** One P4 boot (`rst:0x17`, the capture's USB
reset), no panic or watchdog. Setup completed, the `phone_pipe` task ran, and
the console answered all three `help` probes at its login gate.

**The `=== PREVIOUS CRASH ===` block at boot is not about this image.** It read
`no panic record … phase=running consecutive=1`. This boot's reset was a USB
reset (`reset=usb(11)`), which neither counts a crash nor replays a panic
record. The block prints whenever the consecutive-fault counter is non-zero.
That counter rises on a panic, watchdog or brownout reset and clears once a
boot has run for 60 s (`crashRecordMarkBootHealthy`). Every esptool step stayed
in the bootloader, so no other run came in between. Since the last boot that
ran 60 s there was therefore exactly one fault reset, and the `f9cbb28e` run
that the first esptool step stopped was under 60 s old. The log doesn't say
which fault it was or when: the "brownout or watchdog" text prints for any
leftover count. The device's `/system/sys_logs/crash-history.log` (or
`crashlog`, logged in) names the reset reason of the boot that died. This boot
ran 120 s, so the counter has cleared and the block won't repeat.

**The first records, with Wi-Fi associated throughout:**
- **Phone scan #1** found the phone in 43 ms. **Attempt #1** was cancelled at
  `stage=discovery` with `cause=14(yielded)` after 1,931 ms: the glasses' saved
  reconnect took the setup gate, as on the last image.
- **G2 scan #2** found both temples (saved); the left and right temples
  connected in 237 and 1,097 ms.
- **Phone scan #3** found the phone in 514 ms; **attempt #4** reached `ready` in
  6,942 ms after waiting 126 ms for the setup gate (discovery 5,747 ms).

Evidence is in `tools/p4/companion/private/flash-2026-10-18/`.

### C6 four-link companion installed, 2026-10-18

The user approved installing the four-link C6 companion image through the
programmer route, then putting `0ed5ad61` back on the P4. Main already builds
every C6 with four links (`HW1_C6_BLE_MAX_CONNECTIONS 4` in
`esp_now_hosted_companion.h`, applied by `prepare.py` as
`CONFIG_BT_LE_MAX_CONNECTIONS=4` with four connections reserved at controller
init); only the bench C6 still ran an older bridge that did not report its
limit, so the P4 assumed 3.

| Item | Value |
|---|---|
| C6 image | `private/build-c6/network_adapter.bin`, SHA-256 `8a03598c…e8f9`, Hosted 2.12.13, ELF `f4945a4f1`, built 2026-10-15 from the same bridge source as main (wire version 1) |
| Replaced on the C6 | ELF `8b997b80e` in `ota_0`; full 4 MB backup `private/backups/c6-before-4link-20261018.bin`, SHA-256 `96247e9c…f71c` (`BACKUP_VERIFIED`) |
| C6 writes | app `0x10000`, bootloader `0x0`, partition table `0x8000` (byte-identical layout), OTA data `0xd000`; each `FLASH_READBACK_VERIFIED` |
| P4 backup | full 16 MB `private/backups/p4-before-c6-4link-20261018.bin`, SHA-256 `20331e28…`; it held `0ed5ad61` and the known partition table |
| P4 afterwards | `0ed5ad61` back at `0x10000`, readback byte-identical |

**Deviation from the companion README, now corrected there:** only the
programmer *application* went to `0x10000`. The README also wrote the
programmer's bootloader and partition table, but that table puts NVS at
`0x9000`-`0xF000`, over HardwareOne's NVS and NVS-key partitions, and has no
LittleFS. The programmer booted under the existing bootloader (same chip
revision range, flash size and mode) and reported the C6's 4 MB flash.

**Boot check (120 s).** One P4 boot, no panic or watchdog, setup complete,
`phone_pipe` running, `help` probes answered at the login gate. The C6 log
shows the new ELF and `HardwareOne ESP-NOW bridge registered`. The P4 reports
`C6 up: Hosted 2.12.13 (qualified), bridge present` and no longer prints
`C6 allows 3 BLE links (limit not reported, assumed)`; it prints that warning
whenever the reported limit is below four or missing, so the C6 now reports
four.

**Records, with Wi-Fi associated throughout:**
- Phone attempt #1 yielded to the glasses' boot reconnect at
  `stage=discovery`, as on every boot so far.
- The glasses' scan found both temples; the left connected in 1,945 ms, the
  right in **20,706 ms** (1,097 ms on the previous boot), with no HCI error in
  between. One sample; worth watching now that the controller reserves four
  links.
- Phone attempt #4 reached `ready` in 7,148 ms (discovery 5,720 ms) after
  waiting 26,759 ms for the setup gate behind that slow temple.

**What changes:** with the phone pipe enabled the ring is no longer shut out
(`bleCentralRingShouldYield()` needs a limit below four). Glasses, ring and
phone together beside Wi-Fi remain untested.

Evidence is in `tools/p4/companion/private/c6-4link-2026-10-18/`.

### Ring firmware 2.3.2.0008 profile flashed, 2026-10-18

The user approved this image: the `p4-phone-pipe-camera-off-test` build with
the ring firmware `2.3.2.0008` admitted as its own exact profile, with the
`2.3.2.0007` capability set (connection setup and advStart only, no health),
plus the uncommitted ring-yield fix and its review fixes.

**Why 0008 was admitted:**
- The Even app that installed it (2.3.2 build 243, pulled from the phone and
  decompiled) sends the same pairAuth, systemTime and 12-byte advStart to every
  ring, and never reads the ring's firmware on its connect path.
- The ring had already accepted HardwareOne's pairAuth and deviceInfo.

| Item | Value |
|---|---|
| App image SHA-256 | `f724253f7e382b94b153e57c5fda6d8089f88e6c11981136bc51d22f82292334` |
| Size / headroom | 5,644,544 B / 24.0% |
| ELF SHA-256 prefix | `8e996015d` (printed at boot) |
| Replaced | `0ed5ad61…5ae9`, confirmed by readback; its image is kept in `flash-2026-10-18/` |
| Method | esptool `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks:** the partition table was identical to the build's; secure boot and
flash encryption are off; esptool hash verification passed; the full readback
was byte-identical. NVS, LittleFS and the C6 were untouched.

**Boot check (120 s).** One P4 boot, no panic or watchdog, setup complete, the
console answered all three `help` probes at its login gate. The C6 is still
the four-link image (ELF `f4945a4f1`), with no link-limit warning.

**First records, with Wi-Fi associated throughout:**
- Phone attempt #1 yielded to the glasses' boot reconnect, as on every boot.
- The left and right temples connected in 241 and 310 ms.
- The saved ring reconnected by itself in 2,453 ms, and its setup completed:
  `Connected to saved-ring (setup verified, profile 2.3.2.0008)`. That is the
  pass criterion for the profile (time and advStart both acknowledged).
- The phone then reached `ready` in 6,940 ms (discovery 5,774 ms).

**Four links at once:** glasses (both temples), ring and phone together
beside Wi-Fi, the first time on this bench.

Evidence is in `tools/p4/companion/private/flash-2026-10-18b/`.

### Phone status surfaces and OLED Bluetooth fix flashed, 2026-10-18

The user approved this image ("flash it"): the `p4-phone-pipe-camera-off-test`
build of the main working tree. It adds the phone relay pipe to the status
surfaces (new `BLE_PhoneStatus`: lens Bluetooth → Phone >>, OLED Bluetooth →
Phone Pipe >>, web Phone card, SSE `phone-status`, `phonestatus`). It also
fixes the OLED Bluetooth page, which showed "disabled" in G2-client and
phone-only modes because it tested only the BLE server; its Start/Stop and the
unavailable page's X ran `openble` and switched roles. The ring advStart
saved-address fallback and possiblyWear "maybe" are in the main tree and so in
this image.

| Item | Value |
|---|---|
| App image SHA-256 | `39a7bd98dc5c11f898e6169c8f10b21338f372aec48ee6a1041faad1f8bf54f2` |
| Size / headroom | 5,669,408 B / 23.7% |
| ELF SHA-256 prefix | `ad301b45a` (printed at boot) |
| Replaced | `480c30b1…0222` (the ring session's `flash-2026-10-18f` image), confirmed by readback and kept as `rollback/app-before.bin` |
| Method | esptool 4.12.0 `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks:** the partition table was identical to the build's; secure boot and
flash encryption are off (`SPI_BOOT_CRYPT_CNT` 0); esptool hash verification
passed; the full readback was byte-identical. NVS, LittleFS and the C6 were
untouched.

**Boot check: partial (about 15 s, not 120 s).** One P4 boot (`boot #177`,
`reset=usb(11)`, `crashCount=0`), no panic and no `PREVIOUS CRASH` block,
ELF `ad301b45a`, C6 `f4945a4f1` up with the bridge, G2 client initialized and
the peer auto-reconnect running. Then the user's SerialPortUtility opened the
port and the capture stopped ("multiple access on port"); it was left alone.
Setup completion, the help probes and the link records were not captured.

Evidence is in `tools/p4/companion/private/flash-2026-10-18g/`.

### OLED Phone menu and G2 Status phone row flashed, 2026-10-18

The user approved this image ("Flash 06effab5 to the P4 and capture the full
120 s boot"). It is the same main-tree build as `39a7bd98` plus the OLED
Bluetooth "Phone >>" menu laid out like R1/G2 (Connect, Disconnect, AutoRecon,
then "- Pipe settings -": Start/Stop Pipe, Pair Phone, Forget Phone) and a
"PH: up/down/off" row in the G2 lens Status corner (on the P4 that is 1 list +
5 text children). Like `39a7bd98` it carries the other session's uncommitted
one-worker BLE setup refactor; this is that refactor's first full boot capture.

| Item | Value |
|---|---|
| App image SHA-256 | `06effab5c75ab7a3cb80f85d810ac7c3ccee8e44f83e7458c8208d2262aa128a` |
| Size / headroom | 5,670,032 B / 23.6% |
| ELF SHA-256 prefix | `9a04bf135` (printed at boot) |
| Replaced | `39a7bd98…54f2`, confirmed by readback and kept as `rollback/app-before.bin` |
| Method | esptool 4.12.0 `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz |

**Checks:** partition table identical to the build's; secure boot and flash
encryption off; esptool hash verification passed; full readback byte-identical.
NVS, LittleFS and the C6 untouched.

**Boot check (120 s, complete).** One P4 boot (`boot #178`, `reset=usb(11)`,
`crashCount=0`), no panic, watchdog or `PREVIOUS CRASH` block; setup complete at
19.1 s; the three `help` probes answered at the login gate. Task stacks:
`ble_setup` 3,224 / 5,120 B, `phone_pipe` 2,084 / 8,192 B. The usual early
warnings (SDIO slave reset, LDO voltage 0) and the `RIGHT temple plugin silent`
line ~20 s after the glasses connect appear on every earlier boot too.

**Records, with Wi-Fi associated throughout:**
- `BLE_COEX: setup wait g2 saved ms=2021`, then G2 scan #2 found both temples
  (2 ms); left connected in 65 ms, right in 45 ms (4.5 s apart); `[G2] Connected`
  at 27.8 s.
- The saved ring connected in 69 ms (`wait_ms=0`) at 51 s:
  `Connected to saved-ring (setup verified, profile 2.3.2.0008)`.
- The phone was enabled but not advertising: reconnect scans #1, #3, #4, #5 at
  20, 35, 65 and 95 s each missed (4 s, ~300 reports, 0 matches), matching the
  5 s / 15 s / 30 s-capped schedule.
- No `BLE_SETUP` warnings.

The lens Status corner and the OLED Phone menu are not visible in the log;
they need a look on the device.

Evidence is in `tools/p4/companion/private/flash-2026-10-18h/`
(`boot-console.filtered.log` has addresses, IP and SSID removed).

### Phone connect without Start Pipe and OLED link redraws flashed, 2026-10-18

The user approved this image ("Wait for the starting session's 'phone not found'
change, then build and flash that"). Main-tree build of
`p4-phone-pipe-camera-off-test`. Over `06effab5` it adds: `phone connect`,
`phone pair` and `phone forget` start a stopped pipe (beside a running G2
client, or owning the host when no role runs; never over the BLE server, never
changing the saved mode); the OLED Bluetooth link watcher (the Bluetooth
screens redraw when the BLE role or a glasses, ring or phone link changes); the
pipe keeping its failure label ("phone not found" ...) instead of
"disconnected" when an attempt never reached READY; the `phone ...` / `ring ...`
command renames and the ring address removal; and the G2 one-temple repair
rework (host-tested, not yet hardware-tested, may still change).

| Item | Value |
|---|---|
| App image SHA-256 | `498ac7d60ee4382d59cd6b31f80e906439231427e915a558ec5e67dce27d388c` |
| Size / headroom | 5,671,712 B / 23.6% |
| ELF SHA-256 prefix | `60c5a64f9` (printed at boot) |
| Replaced | `06effab5…128a`, confirmed by readback and kept as `rollback/app-before.bin` |
| Method | esptool 4.12.0 `write_flash` at `0x10000` only, `dio` / 16 MB / 80 MHz; the P4 now enumerates as `/dev/cu.usbmodem11301` |

**Checks:** chip ESP32-P4 rev 3.2; partition table identical to the build's;
secure boot and flash encryption off; esptool hash verification passed; full
readback byte-identical. NVS, LittleFS and the C6 untouched.

**Boot check (120 s, complete).** `boot #180`, `reset=usb(11)`, `crashCount=0`;
no panic, watchdog or `PREVIOUS CRASH` block; setup complete at 18.7 s; the
three `help` probes answered at the login gate. Stacks: `ble_setup` 696 / 5,120 B,
`phone_pipe` 3,116 / 8,192 B.

**Records, with Wi-Fi associated:**
- The phone (advertising again after the app restart) was found in 438 ms and
  reached `ready` in 7,698 ms at 23.7 s.
- `BLE_COEX: setup wait g2 saved ms=6162` (behind the phone's turn); G2 scan #2
  found the temples (12 matches); left connected in 3,840 ms, right in 162 ms;
  `[G2] Connected` at 37.9 s.
- The saved ring connected in 1,550 ms at 52 s, setup verified, profile
  `2.3.2.0008`.
- `RIGHT temple plugin silent` at 57.9 s as on every boot; `LEFT temple plugin
  silent` at 87.9 s is new on this boot (not seen on 18b/18e/18f/18h), possibly
  just the glasses idling; flagged to the G2 repair rework's owner.

Evidence is in `tools/p4/companion/private/flash-2026-10-18i/`
(`boot-console.filtered.log` has addresses, IP, SSID and device-name suffixes removed).
