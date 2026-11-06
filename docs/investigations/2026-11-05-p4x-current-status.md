# Second P4X reliability work: implemented fixes, open qualification

Target: the second P4X-EYE. The other P4 is outside this work.

The deployed application is `phone-close-state-candidate`, ELF SHA-256
`a35d2efc978cf0fed5e4a84c51df3be6c1c6c8849f60af05b2e6c0e8ad8a3af5`.
App-only flash and device digest were verified; settings and other partitions
were preserved. Matching symbols and the original rollback image are retained.

## Implemented and verified

- Preserve the saved glasses mate when AUTO rediscovers one admitted arm.
  The actual registry commit, identity/authority fences and R1 resolver pass
  sanitizer tests; seven deliberate faults are caught. Ring setup and packet
  replies worked with left only, right only and neither arm connected.
- Keep ring clients/handles valid through discovery, disconnect and retirement;
  prevent a disconnected setup from publishing a false successful connection.
- Isolate phone DISCONNECT/CLOSE events by peer address, and end the protocol
  correctly before quarantining an unsolicited failed CLOSE.
- Preserve exact disconnect causes through later retries; enable persistent
  Bluetooth/glasses/ring diagnostics and retain every deployed image's symbols.
- Verify existing retry budgets and setup fairness. No speculative changes to
  radio timing, power or retry policy were made.

Host sanitizer suites and both affected/baseline firmware builds passed.
Four controlled radio-restart trials recovered all four links. Six natural
ring losses on the latest image recovered in8.945–34.905s with application ACKs
and no manual ring reconnect. These are scoped bench results, not walking tests.

## Current limits

The historical stack-protection crash remains unidentified. The user associates
it with phone connection/relay testing. A current repeat did not crash:
31 requests subsequently completed over the phone relay. There were also two
earlier connection refusals while the Studio service was off, one cancelled
before-send request, and one after-send failed outcome whose application cause
was not established. Do not describe the whole run as failure-free.

The old record lacks the crashing task and a usable unwind; the matching old
debug ELF has not been found. Current compiler/DWARF stack analysis did not
establish the old fault's cause. A new reproducible fault or additional original
panic/debug evidence is needed before choosing a crash correction.

The last check had both glasses arms and the phone up; the ring was retrying
bounded connection-establishment failures. Earlier successful recoveries do not
qualify this later ongoing outage. No manual reset was used to hide it.

Manual AUTO learning with one physically unavailable advertiser, genuine
simultaneous range loss/return, and runtime panic-context capture remain open.
The user could not perform the walking test. Full completion is not claimed.

## Deferred at the user's request

- Occasional R1 drops are acceptable with stock firmware; automatic recovery
  and isolation remain the requirement. [Even app comparison](2026-11-05-r1-even-app-follow-up.md)
  must wait for explicit resumption.
- [Why the Studio relay stopped](2026-11-05-relay-service-follow-up.md) is a
  separate deferred host task. Its confirmed URL was left unchanged.

Full evidence and exact open gates: [acceptance checklist](2026-11-04-p4x-acceptance.md).
Private captures/manifests are under `work/second-p4x-investigation-20261104/`.
Logging remains enabled. Two task-owned build caches (~2.1GiB total) are retained
for unfinished crash/qualification work; no temporary audit objects remain.
