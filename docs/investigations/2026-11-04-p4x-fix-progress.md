# P4X-EYE reliability fix progress

The six-item objective remains incomplete. Hardware acceptance has resumed at
the user's request after the software-only checks. Starting source revision:
`9674eb492cd050fb1abdabf4a03fa2017110abca`. Evidence and original findings are
in [the investigation](2026-11-04-second-p4x-bluetooth.md). Private evidence and
build logs remain under `work/second-p4x-investigation-20261104/`.

## 1. Recurring stack protection fault — in progress

The original `c1c6260a4` ELF has not been recovered. Additional searches of the
legacy local HardwareOne and STT project folders also found no matching ELF;
their hashes are recorded in `legacy-elf-search.json`. No unmatched symbols
were used to identify a function.

Source review found two limitations in the old evidence:

- Arduino's RISC-V collector puts the stack pointer first, then scans words for
  executable-looking values. It does not unwind a call chain. The old saved
  trace included that stack pointer as though it were a PC.
- ESP-IDF's hardware stack-guard diagnostic has its own offending instruction
  PC, bounds and current task. Our old record retained only the exception PC.
  Identical old exception PCs/signatures establish the recurring fault class,
  but do not prove that both faults had the same offending instruction.

The new recorder retains task name, RISC-V SP and the hardware stack-guard
PC/bounds in RTC memory and crash history. The early boot output, `crashlog`
text and JSON expose the context; RISC-V candidates are labeled as a stack
scan. Stack-guard signatures include the actual guard PC. A versioned RTC
layout rejects old records instead of interpreting old bytes as new fields.
The original records have already been preserved externally.

This is diagnostic groundwork, not a claim that the underlying stack overflow
or corruption is fixed. The next physical step needs the candidate and matching
symbols, then a reproduction or captured recurrence identifying the task/path.

## Build-input repair discovered during item 1

The ignored Arduino directory was behind the tracked application contracts.
Before changing it, affected files were saved in
`arduino-before-required-patches.tar.gz` with a hash manifest. Restored the
repository's existing URL-OTA removal, panic IRAM/recursion protection, BLE
semaphore-memory and bounded service-search patches, then its phone-pipe patch.

Even after restoration, a full build and the existing SDK lifetime test exposed
a durable-patch omission: G2 callbacks call `getLastDisconnectReason()`, but the
recorded Arduino patch did not supply it or save the reason. The phone-pipe
patch now supplies the accessor and records the reason only after matching the
client's connection ID and peer address, before invoking its callback.
The manifest/checker now covers the header too. The general patch verifier
checks both the accessor and event assignment, so marker counts cannot hide
this omission again.

This dependency repair is tracked in the patch/manifest rather than existing
only in the ignored library. It does not establish which dependency revision
was present in the original board image.

## Verification so far

- Crash capture/consume/render and existing crash-loop boot rules pass with
  ASan/UBSan: RISC-V with stack guard (2,161 checks), RISC-V without it (2,157),
  and Xtensa without it (2,157). Tests include invalid/missing core context,
  null panic information, abort text, old-layout rejection, guard-PC signatures,
  consumption across resets, maximum text lengths and capacities 1–1,024.
- Restored bounded service discovery passes its existing sanitizer regression,
  including the original ring incident (three search requests, two bounded
  waits, no endless wait).
- Existing ring codec/dark-clock/saved-identity tests pass with sanitizers.
- SDK manifest checker passes; its seven checker tests pass. Reverse/apply
  round-trip reproduces exact before/after hashes for all six manifest files.
- SDK lifetime sanitizer test passes 100,000 registrations, exhaustion/wrap,
  foreign disconnect rejection and own-reason publication before callbacks.
- Firmware builds and physical qualification are recorded below as they finish.

### First affected build

`p4-phone-pipe` passed full compilation, linking, profile resolution and image
size checks: 6,141,072 bytes, with 1,284,976 bytes of app-partition headroom.
The hook, bounded string copy, RTOS task getters and hardware-guard getters are
in IRAM in the linked image; the hook's disassembly uses a 32-byte frame.
This checks placement and generated code, not execution during a real panic.

Selected artifacts are in `crash-context-candidate/` under the private evidence
directory (34,879,967 bytes). The binary's embedded ELF hash matches the retained
ELF, and all saved/compressed outputs were checked against the manifest:

- ELF SHA-256: `0bf5ccd028979e95272f7135fa8c15637e13d1e04c8f5692587ac1abc78d1cf0`.
- App SHA-256: `dfc329e9ce9f6a567369286bfa1808c350fbd953666313be29fc85f4a7c9a8c3`.

The `p4-openclaw` baseline (same P4 speech/radio owners, phone pipe disabled)
also passed: 6,044,320 bytes, with 1,381,728 bytes of headroom.
This first candidate was not flashed; the subsequent pair-identity candidate
was deployed as recorded below.

### Item 2 reproduction identified while building

The manual AUTO learning path always commits replacement mask `0x03`, even
when the only observed arm matches the existing saved pair. The registry then
clears the unseen side, leaving ring `advStart` without both identities. A small
reproducer executes the actual production mask assignment and confirms this
decision. This identifies a real source path, but not proof that this manual
operation caused all 355 historical errors. The fix will preserve the unseen
mate only when the observed arm matches its admitted saved identity; a
different/new pair will continue clearing stale addresses.

### Item 2 implemented; hardware acceptance remains

`g2LearnedTargetReplaceMask()` now preserves the unseen slot during manual AUTO
learning only when the live arm matches the admitted saved MAC and, when known,
address type. A newly observed arm still replaces both slots. Explicit left/right
pairing keeps its existing authority semantics. No identities are manufactured.

The new sanitizer regression executes the production matching, selection policy
and ring identity resolver. It covers left-only/right-only rediscovery, repeated
partial attempts, MAC case differences, new pairs, address-type mismatch/unknown,
both arms observed and an incomplete first-time pairing. Existing G2 lifecycle,
peer-registry and R1 protocol/identity suites pass as well.

Both profiles passed full incremental builds after the correction:

| Profile | App bytes | Partition headroom |
| --- | ---: | ---: |
| `p4-phone-pipe` | 6,141,152 | 1,284,896 |
| `p4-openclaw` | 6,044,400 | 1,381,648 |

Selected review candidate: `pair-identity-candidate/` in the private evidence
directory, including matching compressed ELF/map, configuration, dependency
manifest, source patch and new test sources.

- ELF SHA-256: `df298704e375b4c31b10defb3c9006f8fee45d36bb9051864bd9f5ee418b32f6`.
- App SHA-256: `95466dc78382fed2ff2aa712ffd55d71ebd6b15203971b6f59b7dd5f93500beb`.

The first crash-context-only candidate is retained temporarily to isolate or
roll back the identity change during qualification. Both candidate sets can be
consolidated after a working image is physically accepted.

### Recovery review started

Existing sanitizer suites for setup-worker fairness, role-switch reconnect,
reconnect backoff, partial-glasses repair and ring/phone yielding pass. The
shared worker already prevents starvation between queued clients; peer backoff
already grows to 180 seconds for glasses/ring and 30 seconds for the phone.
The ring peer's public UP publication already waits for verified setup.
These existing protections should be retained, not replaced without evidence.
They do not establish real four-link return-to-range recovery.

### Application backup, deployment and first recovery test

The original full factory application partition was read from the second P4X-EYE,
then checked against device flash with esptool's digest verification. Its
SHA-256 is `08a3675b745ba1dadeb9abd939280af4c508545140e3169262229ae6d5f3367d`.
The recovered original app descriptor identifies ELF
`c1c6260a411509f42a094ed99a0623cc313dca26896a9e5bc7fe0e89cc3ce81a`.
This backup preserves rollback bytes; it does not recover its debug ELF.

At 2026-11-05 02:52 UTC, the pair-identity candidate above was written only to
the factory application at `0x10000` and verified. The deployment checked the
target MAC and partition table in the same connection before writing.
Bootloader, partition table, NVS, LittleFS, SD and companion image were preserved.
Private manifests/logs: `original-application-manifest.json`,
`original-application-verify.log`, `deployment.json`, `deployment.log`.

After deployment, both glasses arms were native-promoted (MTU 244), ring setup
was verified and the phone pipe was ready at the same time. The new crash API
returned no new panic record. This establishes initial four-link operation,
not full acceptance of recovery or crash resolution.

A controlled `c6restart` began from that four-link state. The companion returned
with one successful recovery, no transport errors and no missed heartbeats.
The phone first returned to READY, the right-glasses open timed out around 30 s,
and ring setup completed using the saved identities with neither arm connected.
The ring's setup-worker wait was 43,846 ms. Later scans reported neither glasses
arm; the phone briefly needed another reconnect and returned to READY. There
was no new P4 panic. This is an incomplete four-link recovery test, not a pass
and not an out-of-range walking test. Physical reachability/awake state still
needs confirmation; absence from scans does not establish a software deadlock.
Evidence: `live-check-20261105T025557Z/commands.jsonl` and `serial.log`.

Offline instruction matching adds a narrower crash clue: a 64-byte window around
the old exception PC uniquely matches the candidate's `_svfprintf_r` prologue.
The original bytes allocate a 1,152-byte frame before the faulting store.
Shorter windows also match other printf variants, and longer windows do not
match. This is byte-pattern evidence, not exact symbolication, an unwind, or
identification of the crashing task. No underlying crash fix is claimed.

### Item 4: discovery failure cleanup implemented

The service-not-found and notify-characteristic-not-found branches now call the
existing stale-client/GATT cleanup after requesting disconnect. They retain the
client for safe retirement instead of deleting an asynchronously registered
object. Both branches stop after the failed lookup: debug output can no longer
start another discovery and consume an additional timeout on the shared worker.

The existing library discovery sanitizer regression still passes. An additional
harness executes the production ring discovery branches and actual GATT cleanup
over scripted missing service/write/notify combinations, without delivering a
disconnect callback. It verifies immediate offline/stale state, cleared handles,
retained client ownership, one disconnect and no diagnostic rediscovery.

### Item 6: durable ring disconnect causes implemented

The current-client callback now passes the SDK's saved 16-bit HCI reason into
the existing durable DOWN event. Local logical teardown is labeled separately;
it cannot reuse an old link's reason. Existing publication fencing still emits
one DOWN per published UP. The production role-switch regression now also
checks supervision timeout, 0x100, local teardown and duplicate suppression;
all 109 checks pass with sanitizers, including the actual callback's rejection
of a stale client before it reads a reason or clears clock state.

Both latest builds passed profile resolution, compilation, linking and image
size checks:

| Profile | App bytes | Partition headroom |
| --- | ---: | ---: |
| `p4-phone-pipe` | 6,141,104 | 1,284,944 |
| `p4-openclaw` | 6,044,336 | 1,381,712 |

The ring recovery candidate was deployed to the board at 2026-11-05 03:07:52 UTC,
with the same app-only checks and flash digest verification. Evidence is
`deployment-ring-recovery.json` and its `.log`.

- ELF SHA-256: `c3cf4cf46cd79bb7475c4632bdef829446516077290a0870ad633143e176da6e`.
- App SHA-256: `753eb4d5e92f7d81bde789d876e008993c7d3718ce3adfdb5a64240b787acfaf`.

`ring-recovery-candidate/` retains 34,697,472 bytes of selected app, compressed
ELF/map, build inputs, source patch and new tests. Its archive helper verifies
every decompressed file's hash and size, and checks that the app descriptor's
embedded ELF hash equals the retained ELF hash before deployment. This keeps
symbols independent of the disposable compiler cache. Runtime qualification
of the latest candidate is still in progress.

Removed the task-created failed esptool-install virtual environment after its
process exited (8,054,610 logical file bytes). Existing SDK esptool 4.12 performed
the verified backup and deployments. Active build caches and rollback inputs
remain intentionally retained until hardware qualification finishes.

### Live disconnect evidence and close-during-setup fence

The deployed ring-recovery candidate completed ring setup with both glasses
arms absent and the phone READY. A subsequent natural ring drop was recorded
as `reason 0x08` in `/system/sys_logs/events.log`. The verified 1,200-byte log
tail and offset/hash metadata are preserved under
`live-check-20261105T030859Z/verified-ring-disconnect-events-tail.*`.
This physically verifies the new durable HCI field. Ring advertisements were
around -90 to -96 dBm; neither glasses arm appeared in these scans. A later
crash query still had no new panic record; this short observation does not
qualify the original crash fix.

A manual ring close overlapped an automatic OPEN after this drop. Close
deferred native retirement; an unretired-route host recycle followed and the
phone recovered afterward. Source review found that cancellation fenced final
publication but `g2RingClose()` could still transfer/retire the client while
the setup worker was using it. Close now checks the existing setup scheduler's
Ring and RingRetire slots after cancellation/queued withdrawal, before touching
client retirement or releasing the owner. Busy work defers close under its
existing retry contract. Other peers' setup slots do not block ring close.
This fixes the missing lifetime check; elimination of the observed recycle
requires a matching hardware repetition and is not yet claimed.

The new production-close harness covers every busy-slot mask, successful retry
after finish, a busy writer and a refused retirement, for both phone feature
settings. It passes ASan/UBSan along with the existing worker/yield, scan and
owner-lifetime regressions. Discovery and 109-check role-switch regressions
also pass. Both firmware builds pass:

| Profile | App bytes | Partition headroom |
| --- | ---: | ---: |
| `p4-phone-pipe` | 6,141,232 | 1,284,816 |
| `p4-openclaw` | 6,044,480 | 1,381,568 |

Selected **unflashed** candidate: `ring-close-fence-candidate/`.

- ELF SHA-256: `0393f38e376c69cb65adb7be428aa8193c8dc2a6fcea4b6a52595efea47d47e6`.
- App SHA-256: `2879ce2ba116341e3f523be34fe42b2b01e49b6743f18b21e7436ae6a45cc91d`.

The user has requested software checks for now. Further hardware testing and
flashing are deferred; the serial console was released. The deployed image
remains `ring-recovery-candidate`, not this newer close-fence candidate.
The overall six-item goal remains incomplete, especially original crash
resolution, physical partial-pair acceptance and repeated four-link recovery.

### Software follow-up: disconnect-safe discovery and transport publication

Continued software-only review found another definite failure path. Discovery
checked the shared notify handle, then used that same shared field after a
blocking descriptor lookup. The current-client disconnect callback can clear
it in between. A scripted disconnect after a write-handle observation produced
a UBSan null-member-call report. A more direct regression disconnects inside
descriptor lookup, precisely before notification registration.

Discovery now retains setup-local characteristic handles from the current
client's cache, and publishes them only after discovery/telemetry reset under
the existing completion fence. The fence checks request generation and physical
client connection before marking the transport usable. This also prevents a
disconnect during telemetry reset from being overwritten by an unconditional
`connected=true` assignment. The SDK's matched disconnect path does not free
the cache, and the previously added close/setup-idle check prevents close from
retiring that worker-owned client mid-turn. No new owner or retry path was added.

The production-branch discovery harness now covers missing services, handles
and notify capability; notification errors; disconnect inside descriptor lookup,
registration and telemetry reset; a failed reset; an unavailable completion
fence; and a physical disconnect before its callback arrives. It checks cleared
state on failure and handles/publication on success. ASan/UBSan pass with fatal
undefined-behavior reports. Two deliberately faulty variants are rejected:
restoring the shared-handle register call produces a null-member-call report;
removing current-request/connection admission fails the disconnected-state test.
Evidence: `ring-discovery-mutations.log` and `check_discovery_mutations.py`.
Existing ring worker/yield/scan/owner/close and role-switch suites also pass.

Both full firmware profiles passed:

| Profile | App bytes | Partition headroom |
| --- | ---: | ---: |
| `p4-phone-pipe` | 6,141,296 | 1,284,752 |
| `p4-openclaw` | 6,044,544 | 1,381,504 |

Selected **unflashed** candidate: `ring-discovery-fence-candidate/`, 34,711,715
bytes of retained inputs/artifacts with every manifest hash verified, including
the source diff and all new test sources.

- ELF SHA-256: `5a49df26736b74c8d5a00dd1c170625bff44876307b8beefa9c7cd4be424b4de`.
- App SHA-256: `ba5c0798523c8893ff1940679078023b8489547333a2533c35a3576a9f4383fd`.

No serial connection, reset or flash was performed in this follow-up. The board
still has `ring-recovery-candidate`. The null-pointer path is a separate defect;
it does not identify or resolve the historical stack-protection fault.

## Remaining objective

### Software follow-up: packet writer and retry audit

The ring packet writer also re-read its callback-mutable shared characteristic
after checking it. It now captures the handle while holding the existing write
mutex and uses that one handle for the SDK call. The mutex keeps the client
cache alive; the SDK's write method rejects a disconnected client. The callback
can still mark the public link down immediately. No mutex is held by the
callback, no new task was added, and success counters still require SDK success.

A new harness executes the production writer with a scripted callback after
the final shared-state observation (the observed flag value is returned before
the callback's changes are applied to the next access). The pre-correction
source reports a fatal UBSan null-member call; the corrected source passes.
It also covers disconnect during lock acquisition, empty input, offline/stale
state, missing handles/mutex, busy central/write gates, SDK failure and success,
balanced unlocks and packet counters. This establishes source-level behavior
at the injected boundary; the hardware symptom and historical stack fault are
not attributed to it. Evidence: `ring-owner-write-before-fix.log` and
`ring-owner-write-after-fix.log`.

The retry audit found no additional policy defect. `COALESCED`, `BUSY` and
`ROLE_BLOCKED` do not increment launched attempts. Phone retry stays capped at
30 seconds; glasses/ring at 180 seconds. Existing production scheduler tests
pass, including randomized worker fairness (2,759 checks in each compiled
variant) and exclusion of another client's 30-second run from a ring's own
audio-wait budget. This does not prove physical four-link recovery or make a
long queued setup instantaneous. No retry timings/priorities were changed.

Both firmware profiles pass; image sizes/headroom remain 6,141,296/1,284,752
bytes (`p4-phone-pipe`) and 6,044,544/1,381,504 (`p4-openclaw`).
Latest **unflashed** combined candidate: `ring-owner-write-candidate/`,
34,717,873 bytes, all 17 manifest files and embedded ELF identity verified.

- ELF SHA-256: `1bced97dd71a38123badce117f0b5f01ad87b806cafb02013aba0bc2b07cceff`.
- App SHA-256: `8ded2f47dbe3cd331a449e92cc12d2f982ae96842a19818cb6ec1996478ebae9`.

Removed 103,590,148 stored bytes of app/ELF/map files from the never-flashed
`crash-context-candidate`, `ring-close-fence-candidate` and
`ring-discovery-fence-candidate` after verifying the combined replacement.
Their source/build inputs, test snapshots, original manifests and explicit
`RETENTION.json` records remain. Historical mentions above describe what was
retained at that time; these three intermediate folders are now inputs-only.
Both deployed candidates' matching symbols and the verified original device
application backup remain intact. The two normal build caches remain active
task inputs, pending hardware acceptance. No hardware action was taken.

### Outstanding acceptance

1. Recurring stack protection fault: diagnostic context is implemented, but the
   original task/call path and its correction remain unresolved. Matching old
   debug ELF is unavailable; the new recorder has not captured a recurrence.
2. Ring setup with a glasses arm unavailable: source correction and regression
   are implemented; physical partial-pair qualification remains.
3. Four-peer return-to-range recovery: correct any demonstrated stuck or stale
   state and qualify repeated recovery cycles.
4. Ring service-discovery recovery: verify bounded retirement/retry and accurate
   setup-complete status.
5. Retry backoff and setup fairness: prevent unavailable peers from monopolizing
   setup while retaining responsive recovery.
6. Durable disconnect/crash evidence and matching symbol retention: implemented
   capture/reporting and verified symbol archives; real ring HCI persistence
   verified. Panic-context runtime qualification remains unproven without a
   real fault/controlled panic. Continue retaining every deployed image's symbols.

## Workspace ownership

Consolidated software acceptance passed on unchanged input hashes:
`software-acceptance-20261105T033147Z/manifest.json` (ten affected sanitizer
suites, SDK manifest check, diff check; all twelve commands returned zero).
Each log's hash was verified afterward. Deployed pair-identity/ring-recovery
ELFs and the latest unflashed combined ELF were rechecked against their
manifests. No hardware access was used for this audit.

The remaining proof and exact next test sequence are recorded in
[the acceptance checklist](2026-11-04-p4x-acceptance.md). This is not full
goal completion: original crash identification/correction and the hardware
acceptance rows remain open under the user's software-only preference.

Use the current checkout and the ordinary `build-config-p4-phone-pipe` directory,
which was absent at the start, and the normal `build-config-p4-openclaw` for
the phone-disabled baseline. The existing Waveshare build remains untouched.
No extra source/dependency snapshot was created. About 21 GiB was available
before building. Retain selected
firmware, compressed ELF/map, saved configuration, source diff and manifests;
compiler caches remain temporary while this six-item task is active.


## Hardware qualification update — 2026-11-05 04:41 UTC

The current deployed image is `phone-peer-teardown-candidate` (ELF
`7b7ca79eff1b191d4bf20f5e51c256d66cc340a50d70e59a16991a1f10bee346`).
It includes phone DISCONNECT/CLOSE address ownership checks. Native regressions
failed before the guards and pass afterward; twelve sanitizer suites, SDK
checks, and both firmware profiles pass. This fixes a demonstrated software
error, but does not establish that all phone disconnects are corrected.

On this image, a controlled C6 restart recovered phone READY after 13.290s,
both glasses arms after 43.817s, and verified ring setup after 49.270s. Together
with the previous image, four controlled radio restart cycles recovered all
four links. These are bench interruption tests; the user declined walking.
Ring close during an in-flight OPEN deferred safely. Subsequent ring failures
and cancellations initially left the phone READY.

Fresh ring setup also succeeded with both glasses arms disconnected, and with
only the left arm connected: the setup used the saved right-arm identity before
right-arm repair completed. The mirrored right-only test overlapped an existing
ring job and is unqualified. Neither test proves manual AUTO identity learning
with a physically unavailable arm; that acceptance remains open.

A further failure reproduced at uptime 1094970ms: a ring OPEN failed after
29.280s, immediately followed by phone session loss. The phone end bucket is
Connect (7), with no authentication failures. Later phone scans saw hundreds of
advertisements but zero matching phones. This recurrence means the address
checks alone have not qualified the complete phone/ring interaction. The exact
phone teardown event/address and any host/controller involvement remain to be
identified; temporal coincidence is not sufficient attribution. Do not reset
away this evidence. Latest direct check: both glasses arms up, ring connected
and wearStatus ACK at uptime 1189135ms, phone not found; debug queue zero drops.

The next bounded software investigation is the phone teardown path at that
failed ring OPEN, including whether the SDK reports an own-phone event or
another event ends the transport. Retain all deployed symbols and the continuous
capture in `live-check-20261105T042138Z`. The original stack-protection fault
remains unresolved. Full Bluetooth/glasses/ring diagnostics and SD autostart
are enabled; runtime loglink was restored after deployment.


### Follow-up: mirrored partial setup and persistent phone cause

The right-only topology test also passed. Right native promotion completed at
uptime1532077ms; ring setup used the saved left identity at1537793, verified
setup1537891, and received wearStatus ACK1538116, before left repair connected
at1542709/promoted1545707. Joint status then confirmed all four links and
unchanged saved identities. This closes both partial-topology ring setup cases,
while manual AUTO-learning with a physically unavailable arm remains separate.

The native phone terminal latch now records its consumed SDK event, owner
connection ID, raw CLOSE or DISCONNECT reason, GATT status and synchronous write
API refusal. The owner posts phone_link_down when READY ends, and retains the
compact cause in phone stats across later scans/attempts. This removes the
observed evidence loss where a generic not-found result hid the earlier drop.
Native ASan/UBSan tests distinguish foreign/own teardown, own CLOSE0x100,
write refusal, and immutable first-cause cleanup. Production-owner tests verify
retention and the full-value record fits the durable80-byte event detail.
Both feature variants pass owner tests. Both firmware profiles build; current
phone image6141600B/headroom1284448B, baseline6044544B/headroom1381504B.
Targeted evidence: phone-end-provenance-acceptance.json, private work directory.

The diagnostic candidate was app-only flashed/digest verified onto the board at
2026-11-05T04:51:31.775827UTC, preserving settings/data and all other partitions.
Matching ELF39e77b2976105b6f3f891755028f5b21da8f25b347ee8015f0ad3232b3d2fa2b,
appSHAea1f2ec03f0fdadc37656b5b2d5bbf5e78e2391c6d1f7e8a6a2fe5b4b1519da3.
All archived bytes verified, including matching ELF identity and source inputs;
34,741,949stored bytes. Earlier actually deployed symbols and original rollback
remain retained. No disposable source/dependency copies or native crypto builds
remain; ordinary active profile build caches remain for ongoing qualification.
The original crash and recurring physical link failure are still unresolved.


### Stationary hardware and another proven phone state error

The user clarified they remained essentially stationary, with phone/glasses
working around the corner throughout. Movement is not an established cause.
The ring still showed supervision losses with an intact C6 transport; on the
new diagnostic image, the phone remained READY through these ring failures.
A controlled user close/reopen verified the exact phone cause in persisted
/system/sys_logs/events.log and in stats after READY returned. The initial
label read lost for that deliberate stop (error14 remained accurate); the
source correction also tests the explicit Stopped cause, and both owner
variants pass. A GET probe at uptime403091 used module0 instead of system1;
exclude that probe and the following loss from any natural-failure or valid
heartbeat inference. No periodic heartbeat experiment or policy change followed.

A further production native-adapter defect was reproduced in software: an
unsolicited own failed CLOSE set Quarantined but left the protocol READY with
errorNone. The exact native regression failed before correction; stop the
protocol with Connect before quarantining when not already retiring. An
already-retiring close keeps its existing stop cause. The full native GATT
ASan/UBSan suite then passed using the same temporary crypto dependencies;
logs phone-close-error-state-before.log and -after.log. Dependencies removed.
This is a proven state/accounting correction, not yet the explanation for the
ring supervision timeout or the earlier observed phone Connect7 loss.


Final close-state software acceptance passes: native before/after regression,
production-owner tests in both variants, and both firmware builds. Phone image
6141616B/headroom1284432B; baseline6044544B/headroom1381504B. Archived selected
phone-close-state-candidate, 34,743,024stored bytes, matching ELF
`a35d2efc978cf0fed5e4a84c51df3be6c1c6c8849f60af05b2e6c0e8ad8a3af5`.
Evidence phone-close-state-acceptance.json. Superseded never-flashed label-only
candidate is now inputs-only with RETENTION.json; 34,459,866compiled stored bytes
removed after verifying its replacement. Every actually deployed image retains
matching symbols and recovery inputs. Active normal profile caches remain.


The final close-state candidate was app-only deployed and device-digest verified
at2026-11-05T05:04:45.481749UTC on the board, preserving all other partitions.
The new native close-state correction is regression-proven; fresh runtime
verification follows. Original crash and intermittent ring supervision loss
remain open; no radio timing/power/retry settings were changed.


Latest deployed runtime: corrected deliberate-stop record verified in stats,
event ring and persisted events.log; phone reopened successfully. A further
isolation closed the phone at178147ms and withheld reopen until265997ms. Ring
supervision timeout occurred205436ms (+27.289s) while phone was confirmed off
and both glasses remained up. Ring recovered before phone reopen. Therefore a
phone reconnect scan is not required to reproduce the ring failure; this does
not establish that close caused it. Evidence phone-close-only-isolation.json
under live-check-20261105T050514Z. All four links jointly confirmed up again
at295xxx, queue zero drops, no saved new panic. Logging/autostart remain active;
runtime loglink restored. Low-power readback was refused as unsupported on this
ring profile, so actual low-power state remains unknown; no setting was changed.
The ring remains intermittently unreliable despite stationary peers. That root
cause and the historical stack-protection crash remain open.


## User decision — 2026-11-05

Occasional R1 disconnects are acceptable with stock ring firmware. Current
acceptance focuses on bounded automatic recovery and keeping the other peers
usable. The user explicitly deferred investigating how the Even app maintains
its connection or differs from Hardware One. Do not start that work now.
The scope and explicit resume condition are recorded in
[the deferred follow-up](2026-11-05-r1-even-app-follow-up.md).

A single two-minute baseline with G2 links off kept the phone and ring up;
ring uptime145548ms at board uptime518xxx, with no loss during that baseline.
This observation does not establish causality. Glasses restoration was requested
with openg2 saved immediately afterward; no parameters or retry policy changed.


## Current qualification — 2026-11-05

Six natural ring disconnects on the current deployed image recovered without
manual ring/radio intervention:13.617,34.905,8.945,15.017,12.870,9.298seconds
from the callback to verified setup. Every recovery also received an application
wearStatus ACK. Five causes were0x08; one was0x22 (LL response timeout). The
phone retained three READY sessions with only the two intentional test closes;
there was no unexpected phone session end. These stationary results qualify
observed automatic ring recovery, not genuine simultaneous range return.
Evidence: automatic-ring-recovery.json in live-check-20261105T050514Z.

The saved-pair regression now executes the production registry commit, rather
than simulating its mask writes. Actual settings Strings, reconnect snapshot,
address-type retention, partial-topology retry and generation/authority rollback
are checked alongside the real AUTO selection policy and R1 identity resolver.
ASan/UBSan passes. Seven deliberate production mutations (mate erasure, missing
intent/identity/owner checks, missing rollback, lost mate type and stranded retry)
all fail assertions. Production firmware was not changed by this test improvement.
Compact exact inputs and manifest: pair-registry-regression/ in private evidence.

The user identifies phone connection/relay testing as the original crash context;
do not treat earlier adjacent dictation events as the leading reproduction clue.
The user's first retest did not crash but failed: both Terminal and Chat requests
reached the phone and got OPEN_ERR7 before HTTP data. The P4's configured Studio
URL was correct. The user found the Studio relay off, started it, and then
reported the glasses working. Transport records confirmed10 completed Terminal
requests carried over the phone, two earlier before-send failures and no
post-send failure. All four links were up; no new panic record was saved.
Do not call this proof that the historical stack fault is fixed. A direct Mac
mini TCP timeout did not represent the working phone route across VLANs.

No gateway URL, credential, TLS policy or Studio service was changed by this
agent. The user deferred why the Studio service had stopped; see
[the separate follow-up](2026-11-05-relay-service-follow-up.md).
Latest observed free-stack minima after the successful Terminal use were
log_out540B, g2_owner688B, phone_pipe3576B and BTC_TASK3064B. Low measured
margins are investigation leads, not identification of the old crashing task.

## Overnight soak and ring signal strength — 2026-11-05 (Claude, continuing)

Codex's continuous capture (`live-check-20261105T050514Z/serial.log`) ran
unattended until 08:52 UTC (board uptime ~13.6 M ms, ~3.8 h). No panic, reset
or new crash record. Glasses heartbeats and phone continued throughout.

Ring: 24 disconnects in the capture, all HCI 0x08 except one 0x22 and one 0x13.
Every one recovered automatically. While the host was idle after 05:41, outages
lasted 8.6, 24, 14, 31, 1.2 and 24 min; during them the P4 retried direct
connects by saved MAC (30 s each, ~3 min apart, bounded), every attempt ending
at open with 0x100 (never established). No stuck setup or worker starvation.

Every RSSI ever logged for the ring across all captures is
-84 to -96 dBm; the glasses typically log -59 to -77 dBm from the same spot.
The ring link is operating at the receiver-sensitivity edge, which matches
frequent supervision timeouts and long establishment failures while stationary.
This is the leading explanation for ring drops; it is a measured observation,
not yet a controlled A/B result.

The C6 companion's BLE default TX power is +9 dBm
(`CONFIG_BT_LE_DFT_TX_POWER_LEVEL_P9`); the C6 supports up to +20 dBm. On P4X
the hosted backend ignores `bletxpower`. Raising the companion default would
improve only the P4→ring direction. Suggested decisive test before code
changes: place the P4 near the user/ring for ~1 h and compare ring drop rate.

## Ring link parameters and a live stuck state — 2026-11-05 (Claude Code, continuing)

Independent re-count of the overnight soak (`live-check-20261105T050514Z/serial.log`):
21 `[RING] BLE onDisconnect` lines (19 × 0x08 supervision timeout, 1 × 0x22,
1 × 0x13), 22 ring connects completed, 62 failed opens (44 × 0x100 cancelled
after the 30 s open budget, 18 × 0x3e failed-to-establish). Outages after the
host went idle lasted 8.5, 24, 14, 31 and 24 min. Ring advert RSSI across every
capture is −84 to −96 dBm (median −86); glasses adverts −59 to −93 dBm. This
confirms the earlier summary; it is still an observation, not an A/B result.

The ring's connection parameters are visible in every capture: 10–21 s after
each ring connect, a `[G2-?] Conn params APPLIED: int=84 (105.00 ms) latency=4
timeout=600 by=peer(LL)` line follows (20 of 22 connects in the soak). `[G2-?]`
is the GAP handler's label for an address matching neither glasses arm, so this
is the ring's own LL request: 105 ms interval, slave latency 4, 6 s supervision
timeout, which Bluedroid accepts as-is (`l2cble_process_rc_param_request_evt`).
The glasses negotiate 5–6 s timeouts too. A central-initiated update could keep
the ring's interval and latency and lengthen only its supervision timeout (the
glasses already carry that machinery in `setTempleConnParams`); this would make
short fades stalls instead of disconnect/reconnect cycles, but would not help
the 8–31 min outages, where no connection formed at all. Not implemented; this
is a candidate, not a decision. BLE TX power on P4X is the companion's default
(+9 dBm, `CONFIG_BT_LE_DFT_TX_POWER_LEVEL_P9`); `bluetoothHalSupportsTxPower()`
returns false under `CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID`, so `bletxpower`
cannot change it. Raising it needs a rebuilt C6 image and helps only P4→ring.

### The board is stuck right now (found 23:49 UTC, not yet reset)

A 45 s passive read of the board's USB serial port (no DTR/RTS, no reset; script
and log in `live-check-20261105T234858Z-passive/`) at 23:49 UTC found:

- Board uptime ≈ 16.5 M ms, so the P4 restarted around 19:14 UTC. The soak
  capture ended at 08:52 UTC; nothing recorded the restart or its cause.
- `[WARN][RING] WATCHDOG: central job stuck for 12368s — duplicate admission
  blocked; reboot may be required` every 30 s, with `Reseek 'r1-ring':
  admission=coalesced`. The ring's connect job has been in flight since uptime
  ≈ 4,139 s (≈ 20:23 UTC) and never finished.
- `Reseek 'phone' attempt=139: admission=busy` every 2 s: the phone's own job is
  queued (`job` set, step Waiting) and never gets a turn.
- `Reseek 'g2-glasses': admission=coalesced`: one arm up, so the half-pair
  repair owns the missing side. Only `[G2-R]` heartbeats appear (acknowledged
  every 5 s); there are no `[G2-L]` lines at all.
- `E BT_L2CAP: L2CAP - rcvd ACL for unknown handle:0 ls:0 cid:5 opcode:18`
  every ≈ 39 s. cid 5 is the LE signaling channel and opcode 0x12 is an L2CAP
  Connection Parameter Update Request: a peripheral is connected at the
  controller on handle 0 and keeps asking for parameters, while the host has no
  L2CAP link for that handle. The glasses arms use L2CAP for their parameter
  requests (`by=peer(L2CAP)`); the ring uses the LL procedure. Earlier
  captures show this error only as single `cid:4 opcode:27` lines (a late
  notification after a disconnect), never this persistent signaling pattern.

Reading of the setup worker (`BLE_SetupWorkerCore.h`): a queued accessory job
is either run, dropped (fault, recovery block, stale, yield, audio budget) or
waits only for a role transition, and the ring admission would have returned
role-blocked, not coalesced, if a transition were in progress. Nothing else
keeps a queued ring job alive for hours, so the `ble_setup` task is inside a
`run()` that has not returned: a glasses or ring connect body on the worker's
own stack (the phone turn is bounded at 75 s). No `held setup` warning can
appear until that run returns. A 3.4 h stall of this kind is the stuck setup
that acceptance items 3–5 require to be absent; it is a real failure of the
deployed `phone-close-state-candidate`, independent of ring signal strength.

Remaining unbounded waits reachable from those bodies, from the current
Arduino BLE sources: `BLERemoteCharacteristic::readValue`,
`BLERemoteDescriptor::readValue`, `BLEClient::getRssi`, the blocking
`BLEScan::start(duration)` variant, and the client destruction drains
(`m_routingDrainEvt`, `m_callbackDrainEvt`). The firmware's connect bodies use
the non-blocking scan start and do not call `readValue`; the drains are the
open candidates, together with any host-side wait for an event the controller
never delivered (the orphaned handle 0 suggests at least one HCI event was
lost or mishandled). This is a reading of the source, not an identification.

A continuous passive capture is running into
`live-check-20261105T234858Z-passive/serial-passive-cont.log`. Do not reset
or re-flash the board before the state is read out. Needed next, through the
console (login required): `taskstats` (state of `ble_setup`, `g2_owner`,
`ring` tasks), `blestatus`, `blepeers json`, `g2status`, `ring status json`,
`phone status json`, `phone stats`, `c6status` (transport errors, dropped
events, soft recoveries), `crashlog json` (whether the 19:14 UTC restart was a
panic), `events`, and the SD system log around uptime 4,100–4,200 s
(`/sd/logging_captures/system/p4x-a773-bt-hardware-test.log`), which should
show which job was running when the stall began.

### Console readout of the stuck state — 2026-11-06 00:06–00:23 UTC (Claude Code)

Logged in over the no-reset console while the board was still wedged.
Board clock ran about 17 minutes behind UTC; board times below are board clock.

- `taskstats`: `ble_setup` BLOCK (prio 5, 1160 B min free), `r1_owner` and
  `phone_pipe` BLOCK, `BTC_TASK`/`BTU_TASK`/`hciT` alive. `g2status`:
  `state=connecting L=down R=up repair=1/7`. `ring status`: down,
  `connectPending=1`. `phone stats`: 137 scans since boot, 0 found (the phone was
  simply absent). `c6status`: online, 3,532 heartbeats, 0 missed, 0 transport
  errors, 0 dropped events, 0 soft recoveries. `blerecords`: ring connects 50
  (26 ok, 24 failed), glasses one connect per arm since boot.
- `events` / `events.log`: boot #67 (panic) at 18:56:24; `g2_connected L+R`
  18:56:31; `g2_disconnected LEFT, one side up, reason 0x02 unknown` at
  18:57:30 (SD uptime 104,032 ms, 59 s into the left session; the right arm
  never dropped). The ring reconnected 19:00–20:01 (0x08 drops every few
  minutes); the first `ring_reconnect_failed … watchdog blocked after 390s`
  is at 20:11:14, so the ring job went in flight at about 20:05 board time.
- SD system log (`/sd/logging_captures/system/p4x-a773-bt-hardware-test.log`,
  read in 4 KB chunks): ring saved-MAC attempts at uptime 3,959 s and 4,004 s
  ended `stage=open-event gatt=0x0085` after 29.8 s and 25.8 s; the attempt at
  4,049 s ended `stage=open-timeout elapsed=35009ms` (no OPEN event at all);
  the attempt at 4,139,203 ms logged `[RING] Connecting by saved MAC` and
  nothing after it, ever. From 4,319 s on only `Connect skipped — attempt
  already in flight (Ns)` repeats. No `[G2] Scanning`, repair or Bluedroid
  lines appear between 4,139 s and 4,484 s. The SD log holds no IDF-level
  `E (…)` lines at all after this boot (loglink is runtime-only and was lost at
  the 18:56 reboot), so the first L2CAP unknown-handle error cannot be dated;
  it was only visible on serial. The window 4,484–5,060 s was not read.

So the last thing the setup worker is known to have run is ring attempt 4's
`ringPerformConnect`; which call inside it never returned is not established.

### The historical crash is identified: QuickNote worker stack overflow

`crashlog` on the wedged board and `/system/sys_logs/crash-history.log`:
boot #66 died 2026-11-05 18:55:48 (board clock), `Stack protection fault`,
task `qnote`, SP `0x4ff962c0`, guard bounds `0x4ff96310–0x4ff97700` (the SP is
80 bytes below the stack bottom), guard PC `0x40366eb8`, exception PC
`0x40366ed2`, build `a35d2efc9` = the deployed `phone-close-state-candidate`,
whose ELF is retained. Symbolized against that ELF, both PCs are the prologue
of `_svfprintf_r` (`addi sp,sp,-1152`): a printf-family call inside the
QuickNote worker overflowed its 5,120-byte stack (`kWorkerStackBytes`,
`System_QuickNote.cpp`). The two older faults (boots #53 and #56, build
`c1c6260a4`) carry the same scan-word pattern (`0x403a5bd2`, `0x403e3a9c`),
so they are the same path. The event log puts `g2_long_press | ring |
quicknote` at 18:51:22, five minutes before the panic: a QuickNote started
from a ring long-press and died at the end of the note. The deepest frames on
that path are `servicePass` (944 B), `drain`'s 257-byte piece buffer,
`complete` → `QuickNoteStore::create` (three 80-byte paths) and the logging
macros' `vsnprintf`. The user's association with phone/relay testing is not
what the record shows; QuickNote is. Fix candidate: raise the worker stack
(8,192 B is the obvious first value; the task exists only while a note runs)
and log its stack high-water mark when a note finishes, then record a note
from a ring long-press to confirm. Not implemented.

### Debugger incident — the live stall was lost and a panic loop followed

At 00:24:15 UTC I attached GDB through OpenOCD over the built-in USB-JTAG to
backtrace the blocked `ble_setup` task. The attach reset the board
(`boot #68 | reset=jtag(12)`); two further attaches did the same (boots #72,
#77). The task-list walk I read afterwards therefore came from fresh boots and
is discarded; the stall's blocking point is unknown. From boot #69 on the board
panicked every 5–90 s (`Illegal instruction` at `_interrupt_handler`,
vectors.S:437, `csrs mstatus`, on either core; `Instruction access fault` at
PC 0 from a std::function Arduino event callback), boots #69–#94, crashCount
25. A chip-level reset through the USB-Serial-JTAG RTS line at 00:44:25 UTC
(`boot #93 | reset=usb(11)`) did not stop it. OpenOCD's reset path writes an
LP_AON register and forces peripheral clocks on; the LP/RTC domain survives
every reset except power-on, so a power cycle is the next step. Nothing in
the firmware caused this loop. Do not attach a debugger to this board again;
see the memory note. A passive serial capture is running in
`live-check-20261106T004424Z-after-chip-reset/`.

### After the power cycle — 2026-11-06 00:54 UTC

The power-on boot (#105) died once more in the autostart phase; boot #106 has
run cleanly since about 00:54:33 UTC: both glasses arms up, the ring
reconnecting as before (0x08 drops, verified setups), phone link READY,
companion with zero transport errors. Checked at 01:05 UTC.

`/system/sys_logs/crash-history.log` now records the whole loop: 36 deaths from
boot #68 to #105 (board clock 00:07–00:35, UTC 00:24–00:52), two signatures:

- 24 × `Illegal instruction` at `0x4ff20280` = `_interrupt_handler`,
  vectors.S:437 (`csrs mstatus`), always in `IDLE0`/`IDLE1`, 5–90 s after
  boot. Seen only after the debugger attaches, never after the power-on boot.
  Attributed to CPU/LP-domain state the debugger left behind; not firmware.
- 12 × `Instruction access fault` at PC 0 in `arduino_events`, about one
  second after `boot_started`, including boot #105 after power-on. Symbols:
  `NetworkEvents::_checkForEvent` (NetworkEvents.cpp:136) invoking an
  `std::function` whose function pointer read as null, with
  `std::vector<NetworkEventCbList_t>` reallocation code on the same stack.
  Arduino 3.3.5 locks that callback list only when `NETWORK_EVENTS_MUTEX` is
  defined; nothing in the Arduino sources, the project CMake or the P4 compile
  commands defines it, so the dispatcher iterates `_cbEventList` unlocked
  while `WiFi.onEvent` appends to it from other tasks: at boot
  (`wifiCoexMirrorInstall`, HardwareOne.cpp:2178) and in the lazy WiFi init
  (`wifiEventLogger` twice, System_WiFi.cpp:2864–2865) while the hosted
  radio's first events are being dispatched. A reallocation under the
  iterator produces exactly this fault. It surfaced under the rapid reboots
  but is present on every boot; the 2026-10-29 `Load access fault` (boot #21)
  may be the same. Fix candidate: build the Arduino component with
  `NETWORK_EVENTS_MUTEX` defined, and register every handler before any radio
  start. Not implemented.

The SD system log was recreated at boot (69 KB now), so the previous boot's
log, including the unread 4,484–5,060 s window, is gone. The windows already
pulled are saved in `live-check-20261106T000631Z-console/`: `sd-boot.log`
(0–116 s), `sd-stall.log` and `sd-stall2.log` (3,841–4,484 s),
`events-mid.log`, `events-tail.log`, and `crash-history.log` in
`live-check-20261106T005824Z-console3/`.

Still open: which call inside ring attempt 4's `ringPerformConnect` never
returned; why the left arm dropped with HCI 0x02 at 104 s; where the orphaned
controller link came from. A passive serial capture continues in
`live-check-*-passive-after-recovery/`.

### Candidate with both fixes built and deployed — 2026-11-06 01:24 UTC

Changes (working tree, uncommitted, on top of the earlier BLE work):

- `System_QuickNote.cpp`: `kWorkerStackBytes` 5,120 → 8,192; `finish()` logs
  `[QNOTE] worker stack min free N of 8192 B` on every note end. The host
  harness stubs the probe and expects the new size; `test_quicknote_service.py
  --sanitize` passes in both `ENABLE_LOCAL_STT` variants.
- `CMakeLists.txt`: `add_compile_definitions(NETWORK_EVENTS_MUTEX)` before
  `project()`, so Arduino's NetworkEvents dispatcher locks its callback list;
  the compile commands of both P4 profiles carry the define.

Both profiles built from clean (full rebuild forced by the global define):

| Profile | App bytes | Partition headroom |
| --- | ---: | ---: |
| `p4-phone-pipe` | 6,142,864 | 1,283,184 |
| `p4-openclaw` | 6,045,840 | 1,380,208 |

Archived as `quicknote-stack-netevents-mutex-candidate/` (34,797,155 bytes,
every manifest hash verified, source.patch and new tests included).
ELF SHA-256 `8f120019673fd37fd81501fe78cc6088c97920128ac1eca226da6e9e7a53c0fb`,
app SHA-256 `81d1dc7ada99c6a2e3bbe49e442a3bbfa2669e4ee92af915ad346ccded06f714`.
App-only deployed to the board at 01:24:33 UTC after MAC, rollback-hash and
partition-table checks; flash digest verified; other partitions untouched
(`deployment-quicknote-netevents.json/.log`). First boot: both arms up, ring
setup verified, phone READY, companion clean, no crash record.

### A heap corruption on a console-started note — not attributed yet

To exercise the QuickNote fix I started a note from the serial console as the
pairing account (`note start glasses`). The first attempt was refused while
the speech model was being checked. The second was admitted (`phase:
recording, mic: g2`), the STT model loaded, the glasses mic feed started,
`g2ShowText` took its "RIGHT down, falling back to LEFT (may not render —
untested configuration)" path because the right arm's plugin was idle, a
46-byte CREATE-text went to the left arm, and about 1.5 s later the lwIP
thread (`tiT`) aborted in `multi_heap_free_impl` ← `heap_caps_free` ←
`lwip_cyclic_timer`: `CORRUPT HEAP: multi_heap.c:123 detected at 0x480eb440`
(a PSRAM block). Boot #107 died; #108 is running normally. Recorded in
`crash-history.log` with task `tiT`, build `8f1200196`.

What is and is not known:

- The corrupted block's neighbour, the actual writer, is not identified.
- Neither change touches PSRAM: the worker stack is created with
  `xTaskCreatePinnedToCore` in internal RAM, and the mutex define only grows
  the `NetworkEvents` object in `.bss`.
- On the previous firmware the soak shows four Even AI sessions (right arm
  plugin alive) that loaded the same model and started the same mic feed
  without a heap fault; the user's 2026-11-05 ring-hold note also ran to its
  end. No earlier capture exercised a console-started note or the LEFT text
  fallback, so this path has no clean baseline on the old build.
- Attribution therefore needs an A/B: the same console note on the archived
  `phone-close-state-candidate` (ELF `a35d2efc9…`) with the right plugin idle.
  Both images and the original rollback are retained; app-only reflash either
  way is one `deploy_candidate.py` run.

Not verified: the QuickNote stack margin line (no note has finished on this
build), and the NetworkEvents fix beyond a clean boot. The user decides whether
to run the A/B, roll back, or leave the new image in place.

### A/B on the previous image — 2026-11-06 01:38–01:50 UTC

The archived `phone-close-state-candidate` (ELF `a35d2efc9…`) was app-only
flashed back (`deployment-ab-old-firmware.json`), the board left until the
right arm's plugin went idle (as in the failing run), and the same console
note started as the pairing account once `note status` reported available.

- `note start glasses` on the old image: **Stack protection fault** at once,
  task `qnote`, guard PC `0x40366eb8` (`_svfprintf_r` prologue), SP 68 bytes
  below the stack bottom, scan words `0x403a5bd2 0x403e3b9c` — the historical
  crash, now reproducible on demand (boot #111). The command never returned.
- The same note on the new image had passed this point (model loaded, mic
  feed on, text sent) before the separate PSRAM heap fault. So the 8,192 B
  stack moves the worker past the reproducible overflow; its margin is still
  unmeasured because no note has finished on the new image.
- Boot-time race, old image: boots #109, #110 and #112 died at autostart in
  `arduino_events` (null call from `NetworkEvents::_checkForEvent`), i.e. 4 of
  the 7 old-image boots since the power cycle. New image: 0 of its 3 boots so
  far. Suggestive, not proof.
- The heap corruption therefore sits downstream of the old crash on the same
  path and is not caused by the stack size or the mutex; both leave PSRAM
  untouched, and the corrupted block (`0x480eb440`) lies about 258 KB into
  the PSRAM heap, far from the end of PSRAM statics (`0x480ac53c`). Its writer
  is unidentified. A diagnostic build with comprehensive heap poisoning (and
  heap tracing if needed) around one console-started note is the way to name
  it; the console-note / left-arm-text path should be treated as crashing
  until then.

The new candidate was restored at 01:48 UTC (`deployment-quicknote-netevents-
restore.json`, ELF `8f120019…`, digest verified) and boots clean: both arms
up, ring setup verified; the phone was not in range at that check. Board
images now: new candidate on flash; `phone-close-state-candidate` and the
original application retained for rollback. Nothing is committed.

### Heap-poisoning diagnostic image — 2026-11-06 02:06–02:30 UTC

Built `p4-phone-pipe-heapdiag` (new saved configuration from `p4-phone-pipe`
plus `CONFIG_HEAP_POISONING_COMPREHENSIVE=y`). To make that selectable,
`presets/sdkconfig.defaults` gained `CONFIG_HEAP_POISONING_COMPREHENSIVE=n`
(its existing default; the configuration tool only accepts keys listed
there). `System_TaskUtils.cpp` previously refused to compile under poisoning
(`#error`: the task-heap walker equates TLSF block starts with user pointers);
it now takes an explicit path in poisoning or task-tracking builds — no walk,
stacks and TCBs reported as unmeasured (0, the documented meaning), the
creation registry still consulted. Normal builds are unchanged. In addition,
poisoning builds call `heap_caps_check_integrity_all(true)` on every QuickNote
worker pass (200 ms) while a note runs, so a corruption is bracketed to one
pass and the overflowed block is named. Image 6,143,600 bytes; archived as
`heapdiag-candidate/` (ELF `fbf106255611a5672c8233c39e7aa…`), deployed
app-only at 02:08 UTC.

Four console-started glasses notes on that image (right arm plugin idle, the
left-arm text fallback taken each time), each to its 60 s limit: no integrity
failure, no `CORRUPT HEAP`, no fault; the one whose events were read saved
14 words. The corruption did not reproduce. Conditions that differed from the
single failing run on the un-poisoned candidate: the phone link was READY
then and absent (out of range) in all four diagnostic runs, and the failing
note had followed a start refused during the speech-model check — a sequence
the diagnostic runs did not manage to recreate (the check cleared first). Both
remain candidate factors; the writer is still unidentified. The diagnostic
image is kept for a longer watch under normal use with the phone present.

The approved candidate (ELF `8f120019…`) was restored at 02:30 UTC and boots
clean. `debugsystem` was set persistently during one run and reverted.

### QuickNote stack margin measured — 2026-11-06 02:34 UTC

On the restored candidate, with the system debug category enabled for the run
only, a 60 s console note saved 31 words and `finish()` reported
`worker stack min free 2564 of 8192 B`: peak use 5,628 B, more than the old
5,120 B stack, which is why every such note used to end in the stack
protection fault. The new margin is 2,564 B (31 %). No heap fault in this run
either.

Files changed by the two fixes and the diagnostic: `CMakeLists.txt`,
`presets/sdkconfig.defaults`, `configurations/p4-phone-pipe-heapdiag.json`
(new), `components/hardwareone/System_QuickNote.cpp`,
`components/hardwareone/System_TaskUtils.cpp`,
`components/hardwareone/test/host/quicknote_service_harness.cpp`. Nothing is
committed.

## User decision — 2026-11-06 02:40 UTC

Keep `quicknote-stack-netevents-mutex-candidate` (ELF `8f120019…`) on the board.
The poisoning image stays archived (`heapdiag-candidate/`) for the day the
heap corruption recurs; the console-note / left-arm-text path with the phone
linked is the scenario to repeat under it. Still open from the six-item goal:
a bounded recovery for a BLE setup-worker run that never returns (the 4-hour
stall; blocking call unknown, "reboot may be required" is not recovery), the
left arm's HCI 0x02 drop, and the orphaned controller link's origin.

## Bounded recovery for a stuck BLE setup worker — 2026-11-06

### The stall's cause, found in the code

The soak record of the 4-hour stall reads, in order: ring attempt 3 ended
`stage=open-timeout elapsed=35009ms gatt=0x80000000 unreg=completed`, i.e. the
host never delivered any OPEN event; attempt 4 logged `[RING] Connecting by
saved MAC` and nothing else, ever, and the worker never returned. Two code
facts explain exactly that:

- `BLEClient::connect()` (Arduino 3.3.5 plus local patches) takes
  `m_semaphoreOpenEvt` before `esp_ble_gattc_open()` and waits on it with a
  bounded `timedWait()`. The library's `timedWait()` gives the binary semaphore
  back only when the wait succeeds; after a timeout it stays taken. The same
  holds for `m_semaphoreRegEvt` and the 10 s REG_EVT wait.
- `bleQuarantineClientAfterFailedConnect()` (G2_Glasses.cpp) retires and
  replaces a client only after a registration timeout, a stale precheck or an
  outstanding unregister. An OPEN timeout with a completed unregister kept the
  client, so the ring reused it. Its next `connect()` then blocked forever in
  `m_semaphoreOpenEvt.take("connect")`, before any host call: no connect
  record, no result line, a worker that never comes back, every other BLE job
  queued behind it. The glasses path shares the helper and the exposure.

### Fixes

1. Library (`components/arduino/libraries/BLE/src/BLEClient.cpp`, two marked
   hunks, recorded as
   `docs/arduino-local-patches/arduino-local-patches-2026-11-06-ble-connect-semaphores.patch`):
   both timeout paths in `connect()` give their semaphore back, so a reused
   client can connect again; a late event's own give is then a no-op and the
   existing unregister still retires the pending application. Marker minimum
   in `verify_patches.sh` raised 14 → 16; README row added. The phone-pipe
   patch and manifest were regenerated against the new local-patch baseline
   (`work/.../refresh_phone_patch_generic.py`); `check_sdk.py` and
   `verify_patches.sh` pass. `getRssi()` keeps its unbounded wait; it is not
   on the connect path and was left alone.
2. Firmware, same helper: an `OPEN_TIMEOUT` outcome now retires the client and
   arms the host recycle like a registration the host ignored — the belt to
   the library's suspender.
3. Firmware, bounded recovery for any future run that never returns. A
   synchronous connect body on the `ble_setup` task cannot be cancelled from
   outside on FreeRTOS, so the main loop watches its age instead:
   - `BLE_SetupWorkerCore.h`: each slot records `runSinceMs` when it becomes
     Running; `Core::running(client, since)` reports the one running job;
     `hw1setup::overrunDecide(runningMs, reported, policy)` turns an age into
     None / Report / Escalate with `reportMs = 240 s` and
     `escalateMs = 600 s` (the longest legitimate run is a glasses pair,
     about 135 s; the phone turn has its own 75 s ceiling).
   - `BLE_SetupWorker.cpp`: `bleSetupRunning(client, runningMs)` under the
     core lock; nothing else added there, so the host suites that compile the
     worker as is keep compiling it.
   - `BLE_CentralStack.cpp`: `bleSetupOverrunTick()`, called from the main
     loop next to `bleAutoReconnectTick()` inside the Bluetooth block. Past the
     report line it broadcasts and logs one `BLE` system event naming the job
     and its age, and sets the stack lifecycle fault so no new BLE work is
     admitted. Past the escalation line it logs again and calls
     `rebootDevice("ble-setup-overrun", detail, 1000)`, so the next boot's
     event records why. A run that returns in time costs nothing. The ring's
     own 360 s "reboot may be required" line is unchanged; it is now followed
     by the escalation it asks for.

### Tests

- New `test_ble_client_connect_semaphores.py`: splices the real `connect()`
  over a scripted host and a binary semaphore that aborts the moment a
  `take()` would block. Registration never answered, then open never answered
  (the ring case, unregister completed): the second `connect()` on the same
  object must run. Fails on the unpatched library, passes now, with
  sanitizers.
- `ble_setup_worker_harness.cpp` gained `runningAge()`: the running query
  before, during and after a run, a second run's own start, and the policy
  table (no report before 240 s, one report, none twice, escalation at 600 s
  with or without a prior report). 2,779 checks pass with ASan/UBSan and TSan.
- Also passing with sanitizers: `test_ring_phone_yield` (compiles the real
  worker), `test_ble_role_switch_reconnect` (109 checks), `test_phone_sdk_lifetime`,
  `test_ble_coex_records`, `test_quicknote_service`.

### Not done

Not flashed. Not exercised on hardware: the stall is not reproducible on
demand, and the escalation path would need an induced non-returning run to
qualify. The left arm's HCI 0x02 drop and the orphaned controller link remain
unexplained; the heap corruption on a console-started note remains
unattributed.

### Build and archive

| Profile | App bytes | Partition headroom |
| --- | ---: | ---: |
| `p4-phone-pipe` | 6,143,712 | 1,282,336 |
| `p4-openclaw` | 6,046,704 | 1,379,344 |

Both built incrementally on the deployed candidate's tree; the linked image
carries `bleSetupOverrunTick` and `bleSetupRunning` (an earlier build had
compiled the call out because the main-loop file never sees the worker
header's `HW1_BLE_SETUP_WORKER`; the call is now unconditional inside the
Bluetooth block and the function guards its own body). Archived as
`setup-worker-recovery-candidate/` with every manifest hash verified, the
source patch and the new test sources.
ELF SHA-256 `f822cb2446552b8b999edbc46e3a2971541e1a94a74e0f4ecdf98f45743e3819`, app SHA-256 `a7d7707e7ceb62c89776183fdb0d4df4c49610bb363ddf576b4d7b22963bf705`. Not flashed; the board still runs
`quicknote-stack-netevents-mutex-candidate` (ELF `8f120019…`).

### Recovery candidate deployed — 2026-11-06 03:41 UTC

`setup-worker-recovery-candidate` (ELF `f822cb2446552b8b…`) was app-only
flashed to the board at 03:41 UTC after the MAC, rollback-hash and
partition-table checks; flash digest verified; other partitions untouched
(`deployment-setup-worker-recovery.json/.log`). First boot: both glasses arms
up, ring setup verified, phone link READY, companion online with zero
transport errors, no crash record, no overrun line. Board images now: this
candidate on flash; `quicknote-stack-netevents-mutex-candidate`,
`heapdiag-candidate`, `phone-close-state-candidate` and the original
application retained for rollback. A passive serial capture continues in
`live-check-20261106T034441Z-passive-recovery/`.

What this image has not yet shown on hardware: a recovered stall (none is
reproducible on demand; the library fix removes the known cause, so the
overrun watch should stay silent), and a QuickNote end on this exact image
(the worker stack is unchanged from the measured 8,192 B build). Nothing is
committed.

### Overnight soak on the recovery image — 2026-11-06 12:30 UTC

Read back over the console at 12:19–12:30 UTC (`live-check-20261106T122700Z-console20-overnight/`:
status commands, `events`, `blerecords`, `taskstats`, the whole
`/system/sys_logs/events.log` and the new tail of `errors.log`). The Mac-side
passive capture ended at 04:47 UTC when the board was unplugged from the Mac
for the night; it ran on its own power in another room and came back to USB at
12:18 UTC (`power_save_exit`). The ring lay on a nightstand elsewhere, neither
worn nor charging. Board clock ≈ UTC − 27 min in the files below.

System: one boot (#119, the 03:41 UTC flash), uptime 8 h 44 min at readout, no
crash record, no panic, no watchdog, no heap fault, no setup-worker overrun
line. Free internal heap 215,675 B (222,491 B two minutes after boot), free
PSRAM 32,437,156 B (32,560,416 B); companion 6,286 heartbeats, 0 missed, 0
transport errors, heap minimum 159,876 B. `events.log` holds 98 events for the
whole run; between 05:22 and 11:52 UTC the only entries are the hourly `abc`
interval automation.

Links while the board sat still:
- Glasses: connected at boot (03:43 UTC board-time-corrected), never dropped;
  `g2 connects total=1 connected=1` per arm, heartbeat #6278 on both arms.
- Phone: reconnected 03:56 UTC after the post-flash churn and held until
  12:26 UTC — 8 h 30 min — then `lost e=7` while the console readout ran
  (the user was up and moving by then). 3 sessions since boot, 2 failed
  connects, all in the first 20 minutes.
- Ring: after the post-flash churn and one supervision timeout at 04:31 UTC
  (2 min 37 s outage; `link fail 12 s`, `service not found`, `connect FAILED
  20.6 s`, then connected), it stayed connected from 04:33 to 12:06 UTC —
  7 h 33 min with no drop. Wear sensor reported not-worn 05:12–05:22 UTC,
  worn otherwise. Totals since boot: 24 connects, 17 connected, 7 failed, 0
  cancelled.

Morning, once the user was up (not part of the still test): 11:52 UTC a ring
hold on the MAIN lens page started a QuickNote (`note_started hold local`,
STT ran 10 s) but no release arrived; the hijack ended on the 60 s safety
timeout and the hold closed as `75497ms aborted:terminal`. From 12:06 UTC the
ring dropped 7 times in 20 minutes, every one `reason 0x08` (supervision
timeout), reconnecting in 10–54 s each time (one `setup error (timeout)`,
one 5.6 s and one 29.5 s connect failure). A second ring hold at 12:06:48
arrived 2 s after that link drop and closed as `2523ms aborted:overlay`
(the hold path runs over the glasses link, so a ring-link drop does not
block it). The ring was connected again at readout.

Verdict: the device is stable over a night; the new overrun watch and the
retired-client path stayed silent, as they should with the known cause fixed.
Ring drops track the ring moving, not time. Nothing committed; board left on
`setup-worker-recovery-candidate` (ELF `f822cb24…`); console session closed,
port free.
