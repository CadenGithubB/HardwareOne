# R1 Ring Disconnect / No-Reconnect Investigation — 2026-07-28

**Trigger:** overnight run on the v0.99.4-content build (fw string v0.99.3): ring disconnect/reconnect
churn all night, a panic reboot at 07:29:38, and a final disconnect at 08:22:17 after which the ring
never reconnected for ~2h (until manual wake at 10:14).

**Method:** 6 parallel investigation passes (reconnect state machine, BLE worker/crash, power/WiFi,
ring RE docs, TX/anti-replay code audit, log forensics) + adversarial verification of the 6
load-bearing claims. Predecessor doc: `R1_AUTOCONNECT_DISCONNECT_HANDOFF_2026-07-26.md`.

---

## 0. TL;DR — three separate problems, three separate causes

| Symptom | Verdict |
|---|---|
| Ring drops the link (churn, HCI `0x08`) | Ring-side. The RE docs explain exactly one pattern: the ~15s kills. Everything else (2–6 min drops, 31–79 min sessions ending in 0x08, multi-minute refusal gaps) is documented **nowhere** and points at ring state (sleep / wear / battery) we don't model. |
| Panic reboot 07:29:38 (boot #3, crashCount=1) | Unattributable today (no coredump configured). Three ranked candidates, all in the reconnect-churn path; top new suspect: the shared BLE connect worker has a **5 KB stack its author believed was 20 KB** (bytes/words footgun) with a measured 9.4 KB peak. |
| Never reconnects after 08:22:17 | **Not** the handoff's A2 gap. Either (a) a **verified wedge-forever mechanism** — `BLEClient::connect()` with infinite default timeout + three unbounded semaphore waits in the vendored BLE lib; one lost event permanently silences the ladder, Health-Track rescue, *and* manual `ringconnect` until reboot — or (b) the ring was genuinely unreachable for 2h (failed attempts are **unconditionally invisible** in events.log). Logs cannot discriminate; §5 gives the one-command discriminator. |

Autoreconnect itself is healthier than it looks: 18 autonomous recoveries overnight, including one
after a 37-minute outage; reseek gaps quantize cleanly on the 5/15/45/90/180s ladder
(four independent gaps at 48.1s ± 80ms).

---

## 1. The overnight record (19 ring sessions)

Session table (connect → disconnect, lifetime, gap to next connect):

| # | Connected | Dropped | Lifetime | Gap after |
|---|---|---|---|---|
| C1 | 03:12:34 | 03:14:54 | 140.5s | 180.5s |
| C2 | 03:17:55 | 04:36:40 | **78.8 min** | 52.1s |
| C3 | 04:37:33 | 04:37:47 | **14.03s** | 48.1s |
| C4 | 04:38:35 | 04:38:49 | **14.02s** | 38.1s |
| C5 | 04:39:27 | 04:44:38 | 310.8s | 48.1s |
| C6 | 04:45:26 | 04:45:46 | 20.0s | 116.2s |
| C7 | 04:47:42 | 04:48:22 | 40.1s | 38.1s |
| C8 | 04:49:00 | 04:52:49 | 228.4s | 36.1s |
| C9 | 04:53:25 | 05:24:39 | **31.2 min** | 32.1s |
| C10 | 05:25:11 | 06:46:12 | **81.0 min** | **37.2 min** |
| C11 | 07:23:22 | 07:23:38 | 16.0s | 50.1s |
| C12 | 07:24:28 | 07:24:42 | **14.00s** | 66.1s |
| C13 | 07:25:48 | 07:26:44 | 56.1s | 48.2s |
| C14 | 07:27:32 | 07:27:54 | 22.0s | 143.4s *(spans the 07:29:38 panic)* |
| C15 | 07:30:18 | 07:43:15 | 13.0 min | **16.4 min** |
| C16 | 07:59:42 | 08:02:02 | 140.3s | 48.1s |
| C17 | 08:02:50 | 08:06:17 | 206.5s | **8.9 min** |
| C18 | 08:15:08 | 08:21:19 | 370.9s | 44.1s |
| C19 | 08:22:03 | 08:22:17 | **14.04s** | **terminal (≥2h)** |

Key quantitative results (verified):

- **Four sessions died at 14.0s ± 36 ms** (C3, C4, C12, C19) — across both boots and both
  power-save states. Nothing ESP-side has a 14s timer; the determinism is peer-side, keyed to
  something we transmit. *Caveat (verified): events.log stamps on a ~2s drain grid
  (`System_Events.cpp:466`, write-time stamping `System_Debug.cpp:329-333`), so true lifetimes are
  bounded ~12–16s; the millisecond precision is a logging artifact. Still a fixed-timer class.*
- **The naive replay model is statistically refuted:** gap-before-connect vs lifetime correlation is
  ~zero (Pearson −0.14, Spearman −0.13, n=18). The two *shortest* gaps preceded the two *longest*
  sessions (32s→81 min, 36s→31 min); the *longest* gap (37 min) preceded a 16s session. Since our
  setup frames are byte-identical in **every** session (§2), a persistent ring-side replay memory
  would kill every session — it demonstrably doesn't.
- **Reseek gaps quantize on the backoff ladder:** 10 of 18 gaps in 32–52s, four at 48.1s within
  80 ms (deterministic rung + connect latency); 180.5s ≈ full ladder walk (5+15+45+90 + ~25s
  connect). Three gaps exceeded the 180s cap (8.9 / 16.4 / 37.2 min) and **all three ended in
  autonomous recovery with zero user commands** — the ladder retries indefinitely.
- **The night degrades monotonically:** ~85% uptime before the 06:46 drop, ~26% after. No ESP-side
  event aligns with the 06:46 inflection. `ring_worn` edges stop entirely after 05:26:57 —
  consistent with a ring-state change (removed / battery-conserving) preceding the collapse.
- USB-charge noise: weak at best (the 79-min stable session sat *inside* the charge window; two of
  three churn clusters sat outside it).

---

## 2. Do the ring RE documents explain the disconnects? (the user's question)

**Yes — for exactly one pattern.** `g2-kit-unofficial/ble/ring.ts` (Commute773, fetched live)
documents the only ring-initiated disconnect rule in any RE source, verbatim:

> "4-byte anti-replay hash placed at bytes [1..4]. Defaults to four cryptographically random bytes,
> which the ring accepts silently. Only override if you're replaying a captured packet verbatim —
> the ring will disconnect within ~15s if it sees the same hash twice."

How that maps onto our stack (all verified at HEAD):

- Where Commute773 puts random bytes, we put a **deterministic CRC32** (Castagnoli, over the full
  model including the serial — `System_R1_Protocol.cpp:142-143,160-165`).
- `gR1Encoder.resetSerial()` runs on **every** connect (`G2_Ring.cpp:784`, redundantly at
  `:863/:1280/:1300`), so **pairAuth is byte-identical in every session** (serial=1, fixed payload
  `[0x01]`, constant CRC `0xF9531997` — `System_R1_Protocol.cpp:171-179`) and so is **advStart**
  (serial=3, fixed right-temple MAC, `:198-206`). Only systemTime varies.
- The 14.0s logged lifetime ≈ **~15.4s after pairAuth** (the connect event posts ≥1.4s after
  link-up: setup delays 1000+200+200 ms at `G2_Ring.cpp:799,816,839` before the event at `:1154`)
  — matching the "~15s" figure closely.

**But** §1's refutation means the simple story ("ring remembers the hash across reconnects") cannot
be the whole mechanism — identical frames go out every session, yet many sessions live for hours.
Two survivors:
1. The duplicate-kill is **conditional on ring state** we don't observe (wear/sleep/ack refusal), or
2. The ~15s deaths are a **setup/auth watchdog** with the same timer — the ring accepts the link,
   our setup doesn't satisfy it (official app additionally sends config1/config2, linkToGlasses ×2,
   HRV init — handoff §6.2; no doc ties these to lifetime, but none rules it out), and it goes
   silent ~15s in.

The lifetimes alone **cannot discriminate** (verified — both theories predict constant lifetimes,
given our TX is identical every session). §5's instrumentation settles it.

**Everything else is undocumented.** The RE material contains *nothing* on ring sleep, idle,
not-worn behavior, charging-time link policy, connection-parameter preferences, or multi-link
supervision. The 2–6 min and 31–79 min sessions ending in 0x08, and the multi-minute refusal gaps,
have no documentary explanation. The only documented refusal mechanism — R1 is single-central
(`R1_RING_PROTOCOL.md` §1.5) and a temple holding a stale claim blocks the host for 30s/attempt
(§13.4 pitfall 3) — fits the overnight gaps poorly: the documented state persists until a physical
5-tap reset, whereas our refusal windows self-resolved. Note our own setup **does** aim the ring at
the right temple every connect (advStart carries the temple MAC, `G2_Ring.cpp:818-838`), so
ring↔temple entanglement is live even though no HEAD code commands a temple re-seek.

Also verified: **no host→ring heartbeat is ever sent** (`buildHeartbeat` has zero call sites), and
`R1_RING_PROTOCOL.md` §6.19 concluded heartbeat presence made no observable difference in prior
tests. Contradiction flagged for the record: ring.ts says bytes [1..4] are random and "accepted
silently"; our FlutterApp-derived doc says CRC32 there "must be correct" or frames are dropped.
Both cannot be strictly true — the anti-replay inference bridges two incompatible protocol models.

---

## 3. The panic at 07:29:38 (boot #3, reset=panic, crashCount=1)

Landed 104s after the C14 disconnect — inside the reconnect-attempt window, capping a 6-minute
churn cluster. **No backtrace exists anywhere**: coredump is compiled to NONE
(`sdkconfig:1903-1905`, no coredump partition), panic prints to UART only with 0s reboot delay.
The only durable artifacts are the crash counter/reason lines already in events.log.

Ranked candidates (all in the churn path):

1. **Connect-worker stack overflow — bytes/words footgun (confirmed discrepancy).**
   `G2_Glasses.cpp:8921-8924` creates `g2_ble_connect` with `/*stack bytes*/ 5120` while the
   comment above it believes "5120 words = 20 KB", records an observed **~9.4 KB peak** on the
   deepest connect callstack, and warns "connect-time service discovery has genuinely unbounded
   depth on certain failure paths." Actual stack: **5 KB**. Normal ring connects evidently fit;
   deep *failure-path* discovery — exactly what churn produces — plausibly doesn't.
2. **Heap exhaustion from the per-reconnect BLEClient leak.** Every auto-reconnect leaks the entire
   previous client object graph (`G2_Ring.cpp:1041-1052`, no delete anywhere in the module). The
   code *admits* this — the manual path refuses to reconnect under 16 KB free citing "Arduino BLE
   leak per reconnect" (`G2_Ring.cpp:1592-1596`) — but the automatic churn path has **no guard**.
   Boot #2 accumulated ~9 leaked graphs plus failed-attempt churn before the panic.
3. **TOCTOU null-deref.** `onDisconnect` (BTC task) nulls `gRing.writeChar/notifyChar` with no lock
   (`G2_Ring.cpp:859-862`) while writers check-then-deref (`:201-206`); exposure is maximal during
   the post-connect setup write burst against a ring dropping every ~14–22s.

(BTC-stack overflow from the v0.99.4 in-callback additions ranked lower; identical traffic ran for
hours pre-panic.)

---

## 4. The terminal silence (08:22:17 → 10:14): why it "never reconnects"

**Ruled out (verified):** the handoff's A2 gap (failed never-up connect schedules no reseek) — C19
*was* up, so `blePeerNoteLinkLost` re-armed the ladder; the tick reschedules after every attempt
regardless of outcome (`BLE_Peers.cpp:434-445`); Health Track re-arms a reseek every mine interval
while the ring is down (`System_SensorLogging.cpp:1630-1638`). Also ruled out: user-disconnect
stamp (no commands ran), stale `isConnected()` (nothing re-set the flag), millis wrap, power-save
gating (churn demonstrably continued inside power-save; the tick is unconditional,
`HardwareOne.cpp:2336`).

**Two survivors, indistinguishable from these logs:**

### (a) The wedge — mechanism CONFIRMED end-to-end at HEAD
The single persistent connect worker (`G2_Glasses.cpp:8846-8900`) is the only thing that ever
clears `gRingConnectTaskActive` (`G2_Ring.cpp:341-343`), and it does so only after
`ringPerformConnect()` returns. Four unbounded waits can prevent that forever:

1. `BLEClient::connect()` called with **no timeout** → default `portMAX_DELAY`
   (`G2_Ring.cpp:1075/:1083`, `BLEClient.h:91`, `BLEClient.cpp:476`) — wedges if Bluedroid drops
   the OPEN event (the disconnect-event rescue is conn_id-gated, `BLEClient.cpp:536-547`);
2. `getServices` SEARCH_CMPL-with-error breaks **without giving** its semaphore
   (`BLEClient.cpp:636-638,721`);
3. `registerForNotify` error path only logs, then waits forever
   (`BLERemoteCharacteristic.cpp:232-268`; DISCONNECT gives only read/write semaphores, `:513-517`);
4. CCCD descriptor write error path never releases `m_semaphoreWriteDescrEvt`
   (`BLERemoteDescriptor.cpp:230-244`) — disconnects are never forwarded to descriptors.

A wedged worker turns **every** ladder attempt, Health-Track rescue, and manual `ringconnect` into
a silent no-op (`G2_Ring.cpp:1226-1228`, DEBUG-only log), sticks the glasses connect flag too, and
leaks a permanent BALANCED priority hold. `bleConnectShutdown` vTaskDeletes mid-job without
clearing the flag (`G2_Glasses.cpp:8938-8951`) — **only reboot recovers**. Two sibling
semaphore-leak wedges in this same function were already locally patched on 2026-07-27
(`BLEClient.cpp:436-441,469-473`) — the class is proven live on this hardware. And C19 died 14s
after connect, so the ~08:22:22 reseek connected into a *dropping* ring — precisely the
mid-connect-drop scenario that arms paths 2–4.

### (b) Ring genuinely unreachable for 2h
Battery, taken off, out of range. Failed connect attempts are **unconditionally invisible** in
events.log — failure logging is `DEBUG_G2F`-only (`G2_Ring.cpp:1086,1227,1238`) and DEBUG output
goes to the broadcast queue, never to the persisted file (verified; note `g2verbose` does NOT
enable this — it toggles scan-print verbosity only). The ladder provably survived a 37-minute
all-fail streak earlier the same night, so 2h of failing attempts is mechanically possible.
Weak supporting signal: `ring_worn` events ceased after 05:26; no ring-battery telemetry exists in
any log.

---

## 5. Discriminators & recommended fixes (none applied — pending approval)

**Field discriminator (do this first, zero code):** next time the ring is stuck disconnected,
run `ringconnect` (or `blepeers`) **before** rebooting.
- Silent no-op / "Connect task already running" → wedge confirmed (4a).
- Normal connect attempt fires → ring-side unreachability (4b).
Also: check whether the ring reconnected after the 10:14 wake *without* a reboot — if yes, the
08:22 window was (b), not (a).

> **Status 2026-07-28 (same day, post-report):** failure *visibility* implemented and built green
> (feathers3, uncommitted): all ring connect-failure messages promoted from `DEBUG_G2F` to
> `BROADCAST_PRINTF`; new `ring_reconnect_failed` bus event (VERBOSE tier, throttled to first
> failure of a streak + one per 10 min, detail = `reason fail#N elapsed`) persists the retry
> pattern to events.log; the "already in flight" rejection now broadcasts the in-flight age and
> posts a `stuck` event past 120s — making wedge (4a) vs unreachable-ring (4b) readable from
> events.log alone.
>
> **P2 also implemented same day** (built green, uncommitted): explicit 35s timeout on both ring
> `connect()` calls (saved-MAC + `connectTimeout` advert path); vendored-lib patches bounding all
> four wedge waits — `connect` REG_EVT (10s) plus a stale-`rc`-on-timeout fix (a timed-out open
> could return success), SEARCH_CMPL give-on-error + 30s `getServices` bound, `registerForNotify`
> give-on-error ×2 + 10s bound, descriptor read/write give-on-error + 10s bounds (descriptors
> never see DISCONNECT — these guarded the CCCD write inside every ring connect); and a 240s
> in-flight watchdog (`ringConnectGateOpen`, G2_Ring.cpp) that clears a stuck flag, posts
> `watchdog clear after Ns`, and retries — a wedge is now one lost attempt, not
> dead-until-reboot. The per-TX CRC/ack logging below and P3–P7 remain open.

**P1 — instrumentation (the handoff's Phase 1, still unimplemented):** in `ringWriteLocked`
(single TX choke point, `G2_Ring.cpp:193-210`), log `crc32(data[1..4]) + serial + ms-since-connect`
per TX, plus the ring's pairAuth ack presence. One ~14s death with no repeated CRC and no ack →
setup-watchdog confirmed, replay dead. A repeated CRC 10–20s before the drop → replay confirmed.
Add a counter/log on the "Connect task already running" early-return while you're there — it makes
(4a) vs (4b) readable from logs forever after.

**P2 — bound the wedge (cheap, high value):** explicit timeout (~35s) on both `connect()` calls
(`G2_Ring.cpp:1075/:1083`); give-on-error in the three vendored-lib error paths (same shape as the
existing 2026-07-27 local patches); optionally a worker watchdog that clears
`gRingConnectTaskActive` if a job exceeds ~90s. Converts a permanent wedge into one lost attempt.

**P3 — fix the worker stack:** `G2_Glasses.cpp:8922` — 5120 bytes vs intended 20 KB with a
measured 9.4 KB peak. Restore ≥12–20 KB (or re-measure with a high-water probe).

**P4 — clear `gRingTxQ` on disconnect** (`G2_Ring.cpp:854-873` never clears it): stale frames from
session N currently drain at the start of session N+1 *before pairAuth* — a pre-auth stale-serial
TX and a genuine cross-session replay vector independent of the resetSerial one.

**P5 — the BLEClient-per-reconnect leak:** add the 16 KB heap gate (or an actual delete) to the
auto path; the manual path already has it.

**P6 — WiFi (side-finding, unrelated to the ring):** `wifiAutoStart` is **false** on this device —
first-time setup's epilogue forces it false when WiFi wasn't configured in the wizard
(`System_FirstTimeSetup.cpp:872-875`, clobbering even Basic mode's `true` from `:832-834`; looks
unintended). Every `wifi_connected` in all three boots followed a manual `openwifi`. That's why the
device vanished from the network after the panic. Flip the setting (and consider fixing the wizard
clobber).

**P7 — crash forensics:** enable coredump-to-flash (sdkconfig + partition) or keep a serial logger
attached; today a panic leaves nothing but the counter.

**Health logging verdict:** working as configured. Boot #3 autostart created the dated CSV
(`/logging_captures/sensors/2026-07-28/health-2026-07-28T07-29-38.csv`); steady-state TX is one
freshly-built poll every 5s (round-robin over 5 query types, 700 ms floor) + a 4-poll mine burst
every 900s — every frame gets a fresh serial, so health polling is NOT a replay source. The only
suggestive datum: the night's first disconnect came 113s after `healthtrack` was first enabled.

---

## 6. What v0.99.4 changed vs. what it didn't

- NEW: `ring_connected/ring_disconnected/ring_worn` bus events (why this churn is newly visible in
  events.log — it existed before, uninstrumented).
- NEW: refcounted glasses-priority arbiter (RAII-correct on every `ringPerformConnect` return path;
  leak vectors are only the wedged worker and shutdown-mid-job).
- NOT landed: handoff P1 (duplicate-CRC detector) and P2 (failed-connect reseek arming) — P1 is
  now the top instrumentation ask; P2's gap turned out not to be this incident's mechanism but is
  still worth closing for the boot one-shot path.
