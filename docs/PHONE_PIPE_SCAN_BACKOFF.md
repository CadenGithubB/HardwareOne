# Phone pipe: backing off the reconnect scan while the phone is absent

Status: proposal (2026-10-18). Nothing here is implemented. The schedule is
amendment A-3 with the Android team, so a change goes to them first.

## What happens today

While the pipe is enabled and the phone is not advertising, the P4 runs a 4 s
passive scan every 30 s, forever:

```
BLE_COEX: scan #N phone reconnect missed dur_ms=4002(ctl) ... matches=0 scan=60.00/60.00 passive dupfilter=off up_to_s=4
```

**How the timing works** (`BLE_Peers.cpp`):
- **The ladder.** `bleReconnectBackoffMs` gives the phone `kPhoneSteps = {5000, 15000, 30000}`. Glasses and the ring use `{5000, 15000, 45000, 90000, 180000}`.
- **What moves it.** Only an admission that returns STARTED moves the ladder up (`commitAdmission`). A scan that misses ends in `fail("phone not found")`, then `blePeerNoteLinkLost`, which does nothing in the middle of an episode. So a miss neither advances the ladder nor restarts it.
- **After a link loss:** admissions at 5, 20, 50, 80 s, then every 30 s.
- **From boot:** at T, T+15, T+45, then every 30 s.
- **No give-up.** Nothing ever stops the scans.
- **Not committed yet.** `kPhoneSteps` itself is uncommitted; HEAD still gives the phone the glasses' ladder.

**What it costs per hour with the phone absent:**

| Item | Today |
|---|---|
| Scans | 120 |
| Scanner on (60/60 ms window, 100% duty) | 480 s, 13.3% of the time |
| BLE listening the C6 actually grants with Wi-Fi associated | about 40% of that, roughly 5% of airtime (libcoexist tables; not measured) |
| Setup-worker turns spent | 120, each up to about 9 s (5 s scan deadline plus up to 4 s cleanup). A glasses or ring setup that arrives then waits for it. |
| Log | 240 `BLE_COEX` lines. After 8 minutes the shared 16-entry scan record ring holds only phone misses. |

The effect on Wi-Fi, the glasses' mic stream and the ring has not been measured. The study's §4 matrix has not been run.

**Latency when the phone comes back today:**

| Part | Time |
|---|---|
| Wait for the next scan | at most about 26 s, about 11 s on average |
| Finding the phone | median about 0.3 s, 1.17 s at worst with Wi-Fi associated |
| Link setup (firmware-008 record) | about 7 s |

## Options considered

| Option | Change | Absent-phone cost | Latency when it returns | Verdict |
|---|---|---|---|---|
| A. Longer ladder | `{5, 15, 30×8, 60, 120, 180}` s: 30 s for the first ~4.3 min, then up to a 180 s cap | 20 scans/h steady (83% less) | Unchanged for ~4.3 min; then up to 176 s, 86 s average | Right lever, but unsafe on its own (below) |
| A with a Wi-Fi edge trigger | A, plus snapping back to 30 s when Wi-Fi drops | Same | Better on Wi-Fi loss | Misses a boot without Wi-Fi, and Wi-Fi up with the internet down |
| B. Count misses | A, plus a miss streak in the runtime state, fenced by generations | Same | Same | About 100 lines and no gain over the version below |
| C. Shorter scans in the tail | A 2 s scan after N misses | 50% less | Misses 15-26% of returning phones once | Rejected; it also changes the agreed scan profile |

**Why A alone is wrong.** The phone is the P4's fallback network path: the LLM falls back to it when Wi-Fi or the internet fails. A plain 180 s tail makes that fallback slowest exactly when it is needed:
- **Boot without Wi-Fi.** There is no Wi-Fi drop, so no edge for a trigger to catch.
- **Long outages.** Wi-Fi stays away longer than 4 minutes.
- **No internet.** Wi-Fi is up but the internet is not.

It would also slow a phone that is found but fails its attempt, and Android asked for faster retries there (android-009 §2, still open). It would leave a user's Connect press or a pipe restart in the slow tail.

## Recommendation: A, only while the P4 has Wi-Fi, plus one "hold" call

1. **Ladder.**
   - While `WiFi.isConnected()` is true: `{5, 15, 30×8, 60, 120, 180}` s.
   - Without Wi-Fi: today's A-3 `{5, 15, 30}` s.
   - Implementation: sample the bit once per main-loop pass, outside the reconnect lock, and pass it into the admission claim, so BLE_Peers.cpp gains no Wi-Fi code.

2. **`blePeerHoldReconnect(kind, urgent)`.**
   - Same guards as LinkLost (auto on, a saved target, the owner present, no user disconnect), plus `wantReconnect`. It does not bump the intent generation.
   - It sets `launchedAttempts = min(launchedAttempts, 1)`, which gives eight 30 s gaps again.
   - It pulls `dueMs` in to `now + (urgent ? 5 s : 30 s)`, but only when the wait is longer than 30 s and `dueMs` is set. It never pushes a time later, so it is at most one gap faster than A-3 per trigger.

3. **Callers.**

   | Hold | When |
   |---|---|
   | Non-urgent | The phone was heard but the attempt failed before authentication (idle cleanup, when `candidate && !authFailureSeen`) |
   | Non-urgent | A job dropped unrun ("setup turn ended unused", "central link capacity unavailable") |
   | Urgent | `phonepipe connect` |
   | Urgent | A successful `phonePipeStart` |
   | Urgent | Wi-Fi down for 10 s or more (debounced) |
   | Urgent | An LLM request with no ready path while the pipe is active |

   Only a clean miss, or an authentication failure, moves the ladder up.

4. **Result.**

   | Situation | Behaviour |
   |---|---|
   | Bench (Wi-Fi associated, phone absent) | 20 scans/h, 83% less |
   | No Wi-Fi | Exactly today's A-3 |
   | Heard but failing | Retries every 30 s or less |
   | A user action or an LLM request | Retries within 5 s |

About 60 lines, plus tests.

## The A-3 amendment to send

"While the P4 has Wi-Fi: 5 s, 15 s, then 30 s for 8 attempts (about 4 min),
then 60 s, 120 s, 180 s capped. Without Wi-Fi: unchanged. Back to the 30 s
stage, within 5 s, on Connect or pipe start on the P4, on Wi-Fi lost for 10 s,
or on an LLM request with no ready path. A found phone whose attempt fails
before authentication stays at 30 s. Unchanged: the scan profile (60/60 ms,
passive, no duplicate filter, 4 s, stop at first match), the 30 s
authentication cooldown, FW-1."

Points to settle with Android in the same note:
- **android-009 §2.** Under this proposal, found-but-failed attempts retry within 30 s.
- **FW-1 flows.** First unlock after a reboot, reopening after Force stop, and tap-to-resume can all follow a long absence. On Wi-Fi they may now wait up to one tail gap. Do they need a nudge from the phone side?
- **Android code.** None is needed: the phone advertises continuously, with no Wi-Fi gate.
- **Qualification run:**
  - the phone away for 10 minutes or more, then back, once with Wi-Fi and once without;
  - a Wi-Fi-loss run.

## Can ship now, with no amendment

- **Log noise.** Keep per-peer scan records, so phone misses stop pushing glasses and ring records out of the shared ring. Or fold repeated phone misses into one log line with a count. Most of the serial noise is this.
- **Text fixes.** The `phonepipe scan` usage text; and the coexistence study's "every 34 s" should read 30 s (the gap is measured between admissions).

## Before sending

- **Measure first.** Run one cell of the study's §4 matrix: the pipe enabled with the phone absent, against the pipe stopped, with the glasses connected and Wi-Fi associated. Measure G2 mic packets/s and HTTP round-trip time. The design notes say to tune the schedule only from hardware evidence.
- **Order.** Land it after the one-worker BLE refactor has passed its hardware test, as its own qualification item. Build all seven lanes and add an inline stub in `BLE_Peers.h` for no-Bluetooth builds.

## Host tests and mutations (when implemented)

**Tests:**
- **Ladder** (`ble_reconnect_schedule`):
  - the phone's gaps with Wi-Fi are `15, 30×8, 60, 120, 180…` s, and `{15, 30, 30…}` s without;
  - glasses and ring are unchanged;
  - 29 admissions in the first hour after a loss, then 20 an hour.
- **Episode** (`ble_peer_registry`), an hour of 100 ms ticks:
  - admissions land exactly at the expected offsets;
  - a LinkLost in the middle of an episode changes nothing;
  - LinkUp, boot and reseek restart the ladder.
- **Pipe** (`phone_runtime`):
  - a clean miss makes no hold call; a heard-but-failed attempt makes a non-urgent one;
  - dropped jobs make a non-urgent hold;
  - Connect and Start make an urgent one;
  - the Wi-Fi debounce works, and its tracker keeps updating while the pipe is off.
- **Hold** (`ble_peer_registry`):
  - attempts become 1, giving eight 30 s gaps;
  - the pull-in happens only when the wait is over 30 s;
  - nothing is ever pushed later, and `dueMs == 0` is left alone.
- **LLM** (`llm_http_transport`): no ready path with the pipe active calls the urgent hold.

**Mutations that must each fail a test:**
- revert the ladder;
- add or drop one 30 s rung;
- a 120 s or 300 s cap;
- use the phone's steps for every kind;
- ignore the Wi-Fi bit;
- the hold sets attempts to 2;
- an unconditional pull-in;
- count auth failures as heard;
- remove the LinkLost call after a miss;
- level-triggered Wi-Fi instead of debounced.
