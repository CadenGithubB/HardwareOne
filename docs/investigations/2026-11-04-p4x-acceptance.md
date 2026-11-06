# P4X reliability acceptance — open

Goal status: active; hardware testing resumed at the user's request after the
other agent disabled Bluetooth on the STT P4. Software qualification is complete
for the combined candidate; the original crashing task/path and physical
acceptance remain unresolved.

User clarification, 2026-11-05: occasional R1 disconnects are acceptable while
using its stock firmware. Require bounded automatic recovery and isolation of
the other links, rather than uninterrupted R1 uptime. The stock Even app
comparison is explicitly deferred; see [the follow-up note](2026-11-05-r1-even-app-follow-up.md).

Target: the second P4X-EYE (its identity is recorded in the private archive). The other board is outside
this test. A fresh baseline confirmed both glasses arms, verified ring setup
and phone READY before app deployment began. Hardware test results follow in
the progress record; an attempted test is not a passing result.

## Current images

| Role | Selected image | ELF SHA-256 |
| --- | --- | --- |
| Previous combined; three four-link radio restart cycles passed | `ring-owner-write-candidate` | `1bced97dd71a38123badce117f0b5f01ad87b806cafb02013aba0bc2b07cceff` |
| Previous phone guard candidate; another all-four restart passed, but phone loss recurred after failed ring OPEN | `phone-peer-teardown-candidate` | `7b7ca79eff1b191d4bf20f5e51c256d66cc340a50d70e59a16991a1f10bee346` |
| Previous diagnostic image; persisted cause and retention across reconnect verified on hardware | `phone-end-provenance-candidate` | `39e77b2976105b6f3f891755028f5b21da8f25b347ee8015f0ad3232b3d2fa2b` |
| Now deployed/flash-verified; failed CLOSE ends protocol before quarantine, accurate deliberate-stop label | `phone-close-state-candidate` | `a35d2efc978cf0fed5e4a84c51df3be6c1c6c8849f60af05b2e6c0e8ad8a3af5` |

The latest candidate includes the saved-pair correction, expanded crash context,
durable ring HCI causes, discovery cleanup, close/setup lifetime fencing,
disconnect-safe discovery/publication, retained packet-write handles and native
phone DISCONNECT/CLOSE peer-address ownership checks.
It also retains READY-session end provenance in the durable event and stats.
Both deployed candidates' symbols, latest candidate symbols and original
verified application rollback are retained privately under
`work/second-p4x-investigation-20261104/`.

## Software gate — passed

Consolidated evidence: `software-acceptance-20261105T041809Z/manifest.json` in
the private evidence directory. It records source/test hashes, commands, exit
codes and log hashes; inputs remained unchanged throughout the run. All twelve
affected sanitizer suites, the SDK manifest checker and `git diff --check`
passed. The suite covers crash capture/rendering, saved-pair identity, SDK
lifetime, peer registry, glasses lifecycle, reconnect backoff, worker fairness,
ring discovery/write boundaries, role switches, ring/phone yielding, native
phone GATT peer ownership and the phone protocol core.

Latest targeted diagnostic verification is recorded in
`phone-end-provenance-acceptance.json`, followed by
`phone-close-state-acceptance.json`: native GATT and production-owner
ASan/UBSan tests pass, including first-cause retention and bounded rendering.
Both latest firmware builds passed feature resolution, compilation, linking
and partition checks: phone-pipe image 6,141,616 bytes, baseline 6,044,544.
The latest archive's 17 manifest files and embedded ELF identity were verified.
Host BLE/RTOS boundaries are scripted; these results do not prove physical
reachability, radio timing or execution during an actual hardware panic.

## Requirement-by-requirement acceptance

| Item | Proven now | Required before completion |
| --- | --- | --- |
| 1. Recurring stack-protection crash | Two original saved faults; improved task/SP/guard capture passes software tests | Identify the offending task/path using matching original symbols or a new complete fault record, correct that path, and exercise the triggering operation. A short panic-free boot is insufficient. |
| 2. Ring with one glasses arm missing | Production AUTO policy, actual registry commit and R1 resolver pass integrated sanitizer tests; seven deliberate faults are rejected. Fresh ring setup/packet ACK passed with no arms, left only, and right only, using saved identities before repair completed | Manual AUTO rediscovery with a physically unavailable arm remains distinct: check both saved addresses before/after for left-only and right-only learning. |
| 3. Four-link return to range | Previous combined image recovered all four after three C6 restarts in 21.297/47.759/25.131 seconds; every cycle had joint status confirmation | Genuine range loss/return remains untested: user cannot perform the walking test now. Validate the newly found phone peer-ownership fix on hardware too. |
| 4. Ring discovery recovery | In-flight OPEN close deferred without host recycle; interrupted discovery retired safely and later received a packet ACK. Latest image completed six natural recoveries in 8.945–34.905s, each with setup and application ACK, without manual ring/radio intervention | Bench cases qualified. A later outage remains unqualified: repeated OPEN attempts terminate normally with establishment failures and retry backoff; current peripheral availability is not established. Do not label it a proven host deadlock or a passing recovery. |
| 5. Backoff and fairness | Production budgets/worker fairness tests pass. Latest image kept the phone session up through ring failures; two recorded phone ends were deliberate test closes. Repeated failed ring OPENs returned within their bounds and gave the worker back | Bench cases qualified; genuine simultaneous range return remains part of item 3. No speculative backoff or radio-parameter changes were made. |
| 6. Durable diagnostics/symbols | A natural ring supervision timeout persisted as HCI 0x08; matching deployed/latest symbols verified; crash context software tests pass | Verify panic-context capture during a real/bench fault, then confirm the saved record and exact matching ELF can identify the failing path. |

## Next hardware sequence, when testing resumes

1. Preserve a fresh status/log snapshot and verify the target identity and
   partition table before deploying the combined app. Use the existing verified
   app-only deployment and rollback path. Do not substitute a full flash bundle.
2. Start continuous private serial capture. Establish the baseline with
   `blepeers json`, `g2status`, `ring status json`, `phone status json`,
   `c6status`, `crashlog json` and `taskstats`. Require both glasses arms up,
   ring setup verified and phone READY before a four-link recovery test begins.
3. For each partial-pair case, retain `blepeers json` before and after
   `openg2 auto`. Use one known awake arm, then the other; require the unseen
   saved mate to remain unchanged and ring setup to succeed.
4. Exercise repeated genuine range-loss/return cycles. Explicit `closeg2`,
   `closering` and `closephone` suppress automatic recovery by design and are
   not substitutes for this test. Record time from confirmed reachability to
   each peer's usable state and all setup waits/failures. Preserve a stalled
   stage before intervening; do not hide a failure by repeatedly reconnecting.
5. Interrupt ring setup and repeat close during its OPEN/discovery turn.
   A deferred close may require its documented retry after setup finishes;
   it must not retire a still-used client or require an unnecessary host reset.
6. If a panic occurs, preserve the full transcript and saved crash history
   before another flash/reset. Match the full ELF identifier; retain task,
   SP, guard PC/bounds and reset/boot ordering. RISC-V candidates are a stack
   scan, not an unwound call chain. Board clock values were previously about
   22 minutes behind the host, so retain raw clocks and uptime separately.

Full progress and limitations: [fix progress](2026-11-04-p4x-fix-progress.md).

## Remaining evidence needed

The user identified phone connection/relay testing as the historical crash
context. The latest repeat worked after the Studio relay was started, without
reproducing the P4 panic. An offline compiler/DWARF audit did not identify the
historical task: the current glasses-owner frame is1584B and logger frame448B,
but these are not full call-chain bounds. The old saved scan words are heap/data
addresses rather than executable return addresses. Do not infer a crashing task
or prescribe an arbitrary stack increase from them.

Further crash correction needs a reproducible fault or additional original
debug/panic evidence. Physical AUTO-with-one-advertiser, genuine range return,
and actual panic-context capture remain unperformed. Working relay traffic and
a panic-free soak are useful evidence, not proof of those remaining gates.
