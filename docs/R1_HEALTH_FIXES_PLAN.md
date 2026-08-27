All five sit on the same shape of problem, but the framing only half-holds, so state it precisely rather than prettily. Fixes 1, 2 and 3 are literally "the data already arrived, we already decoded it, and then we threw it on the floor": the Health Track mine stops one step short of the query that carries battery and wear; the ring tells us how old every sample is and the logger drops it; the ring sends steps and kcal and we format them into a debug string that goes out of scope. Fix 4 bends the framing — the data arrives fine, but it is merged onto the wrong end of the array and behind a gate that latches shut after eight samples, so it is "we collect it and then file it wrong". Fix 5 doesn't fit at all and shouldn't be sold as if it does: it adds no vitals, it makes one fact already on the wire (the CRC32 in every frame we transmit) visible so you can test a hypothesis about why the ring link dies. Do Fix 5 first anyway, because its answer changes whether Fix 3's traffic is safe to add.

---

## Fix 5 — TX duplicate-CRC detector (do this first)

**What's broken.** Nothing, yet — that's the point. The R1 ring drops the BLE link with `rsn=0x08` after ~19 s or ~2 min, and the leading theory is that the ring kills any peer that sends the same envelope CRC32 twice. Two places in our code can do exactly that, and today there is no way to see it happen.

The two replay sources are real, in [G2_Ring.cpp:214](components/hardwareone/G2_Ring.cpp:214):

```cpp
static void ringDrainPendingLocked() {
  uint8_t  buf[R1_MAX_FRAME];
  uint16_t len = 0;
  while (ringTxQPop(buf, &len)) {
    if (!ringWriteLocked(buf, len)) {
      (void)ringTxQPushFront(buf, len, /*coalesceKey=*/0);   // ← exact bytes, re-sent
      break;
    }
  }
}
```

If `writeValue()` returned false but the ATT write actually landed, that retry is a byte-for-byte replay. Separately, [G2_Ring.cpp:784](components/hardwareone/G2_Ring.cpp:784) calls `gR1Encoder.resetSerial()` on every reconnect, and the serial is *inside* the CRC input — so pairAuth on link N+1 is identical to pairAuth on link N.

**Why it matters.** You cannot fix a drop you cannot attribute. Right now every `rsn=0x8` looks the same whether the ring killed us for a replay or simply went silent and hit supervision timeout. This fix produces one log line that tells those two apart.

**The change.** Detection only, entirely file-static inside [G2_Ring.cpp](components/hardwareone/G2_Ring.cpp) — no header, no flag table, no settings, no web surface.

New block between `ringTxQPushFront()` and `ringWriteLocked()`:

```cpp
static constexpr size_t   RING_TXDUP_SLOTS  = 16;
static constexpr uint32_t RING_TXDUP_AGE_MS = 30000;
struct RingTxDupEntry { uint32_t crc32; uint32_t ms; uint16_t epoch; };
static RingTxDupEntry gRingTxDup[RING_TXDUP_SLOTS];
static uint8_t  sRingTxDupNext   = 0;
static uint16_t sRingTxLinkEpoch = 0;   // ++ per standard setup (serial reset)
static uint16_t sRingTxDupCount  = 0;
static uint32_t sRingTxDupLastMs = 0;
static uint32_t sRingLastRxMs    = 0;

// Caller holds gRing.writeMutex.
static bool ringTxDupNote(uint32_t crc, uint32_t* ageMs, uint16_t* epoch) {
  const uint32_t now = millis();
  bool dup = false;
  for (size_t i = 0; i < RING_TXDUP_SLOTS; i++) {
    const RingTxDupEntry& e = gRingTxDup[i];
    if (e.ms == 0 || e.crc32 != crc) continue;
    if ((uint32_t)(now - e.ms) > RING_TXDUP_AGE_MS) continue;  // rollover-safe
    *ageMs = (uint32_t)(now - e.ms); *epoch = e.epoch; dup = true; break;
  }
  RingTxDupEntry& slot = gRingTxDup[sRingTxDupNext];
  slot.crc32 = crc;
  slot.ms    = now ? now : 1;      // 0 is the "unused slot" sentinel
  slot.epoch = sRingTxLinkEpoch;
  sRingTxDupNext = (uint8_t)((sRingTxDupNext + 1) % RING_TXDUP_SLOTS);
  return dup;
}
```

Then inside `ringWriteLocked()`, **after** the second liveness re-check and immediately before `writeValue()` — so mutex-timeout and stale-client paths never record a phantom entry:

```cpp
  if (len >= 5) {
    const uint32_t crc32 = (uint32_t)data[1] | ((uint32_t)data[2] << 8)
                         | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
    uint32_t dupAgeMs = 0; uint16_t dupEpoch = 0;
    if (ringTxDupNote(crc32, &dupAgeMs, &dupEpoch)) {
      sRingTxDupCount++; sRingTxDupLastMs = millis();
      DEBUG_G2_PROTOCOLF("[RING] TX duplicate CRC=0x%08lX len=%u age=%lums link=%s dup#%u",
                         (unsigned long)crc32, (unsigned)len, (unsigned long)dupAgeMs,
                         dupEpoch == sRingTxLinkEpoch ? "same" : "prev",
                         (unsigned)sRingTxDupCount);
    }
  }
  const bool ok = gRing.writeChar->writeValue(const_cast<uint8_t*>(data), len, false);
```

Three smaller hunks: stamp `sRingLastRxMs = millis();` next to `gRing.packetsReceived++` in `ringDumpFrame()`; bump `sRingTxLinkEpoch++` right after the `resetSerial()` at :784; and put the counters on the drop line at :865, which prints with no debug flag set:

```cpp
      const uint32_t upMs = gRing.connectedSince ? (millis() - gRing.connectedSince) : 0;
      BROADCAST_PRINTF("[RING] Dropped BLE link — ring is no longer connected "
                       "(up=%lu.%03lus rxAgo=%ldms txDup=%u dupAgo=%ldms)", ...);
```

Deliberate inversion of the handoff doc's Phase-1 spec: **do not clear the history on disconnect.** The doc says to, but the cross-link pairAuth replay is hypothesis (2), and clearing would hide exactly the case the ~19 s post-reseek drops point at. Entries age out on time and the log says `link=prev`.

**Other files that must change.** None. No new exported symbol, no `G2_Ring.h` edit, no X-macro entry — bit 74 `G2_PROTOCOL` already documents "envelope TX/RX, CRC" and `DEBUG_G2_PROTOCOLF` exists at [System_Debug.h:484](components/hardwareone/System_Debug.h:484).

**Downstream.**

| What | Effect |
|---|---|
| `writeValue()` at [G2_Ring.cpp:206](components/hardwareone/G2_Ring.cpp:206) — the file's only GATT write | **benign** — ~30 byte-ops before the send; return value and frame untouched |
| `ringDrainPendingLocked()` retry at :218 | **benign** — deliberately unchanged; the detector must observe existing behaviour |
| `"[RING] Dropped BLE link"` string | **benign** — grepped the tree; only other hits are prose in [docs/R1_AUTOCONNECT_DISCONNECT_HANDOFF_2026-07-26.md:132](docs/R1_AUTOCONNECT_DISCONNECT_HANDOFF_2026-07-26.md:132). No parser |
| `ringstatus` / `g2RingGetStatus` JSON schema 1 | **benign** — untouched on purpose; adding `txDup` would ripple into APP_JSON_CONTRACT, WEB_PAGE_API_MAP and `parseRingStatus()` for a temporary instrument |
| `ringPushStatusEvent("disconnect")` SSE on the next line | **benign** — untouched; /bluetooth page unaffected |
| Internal DRAM `.bss` | **benign** — 16 × 12 B + 14 B ≈ 206 B, deliberately not PSRAM (scanned on the TX path) |
| [docs/R1_AUTOCONNECT_DISCONNECT_HANDOFF_2026-07-26.md](docs/R1_AUTOCONNECT_DISCONNECT_HANDOFF_2026-07-26.md) §7/§8 | **needs update** — record that Phase 1 landed and that "clear on disconnect" was intentionally inverted |

**Risk.** Low, but be honest about two things. `ringTxDupLookup()` runs on the notify task while `ringTxDupNote()` runs under the write mutex — unsynchronised by design; aligned 32-bit loads are atomic on Xtensa, so the worst case is a stale diagnostic line. And the epoch only increments in `ringRunStandardSetup()`, so a connect that aborts before setup (no `writeChar`) leaves the epoch unchanged and a later pairAuth is labelled `link=prev` against a session that never transmitted. Read that label as "an earlier attempt", not "the previous link definitely sent this". Nothing here can change TX behaviour: the detector cannot return early and cannot mutate the buffer.

**How you'd know it worked.** Prove the instrument before trusting a negative. `debugg2protocol 1`, `ringconnect`, wait for `[RING] TX pairAuth ser=1`, then `ringdisconnect` and `ringconnect` again **within 30 s**. The second connect *must* print `TX duplicate CRC=... link=prev dup#1`. If it doesn't, the detector is broken — stop. Then leave it connected 10 minutes with polling running and confirm **zero** dup lines (every poll rebuilds with a fresh serial). Then the real run: `debugg2 1` overnight, and read the drop line. `dupAgo` in the 10–20 s range with `txDup` incrementing at the drop ⇒ hypothesis confirmed. `rxAgo ≥ ~30 s` with `txDup=0` ⇒ the ring went quiet on its own; close the CRC lead.

**Size.** Under an hour.

---

## Fix 1 — Health Track mine stops one step short

**What's broken.** The timed Health Track mine walks the poll cursor with a hardcoded `4` while the ladder has five steps.

[System_SensorLogging.cpp:1414](components/hardwareone/System_SensorLogging.cpp:1414):

```cpp
      if (sHtMineCursor >= 4) {          // ← literal, not the constant
```

Cursor 4 is the `deviceStatus` query, and its notify reply is the **only** thing that writes `gR1Cache.battery` and calls `ringNoteWear()` ([G2_Ring.cpp:488-501](components/hardwareone/G2_Ring.cpp:488)). Every other burst site — the on-demand burst 60 lines above, the lens Health page, the OLED page, the round-robin logger — uses `G2_RING_POLL_VITAL_COUNT` correctly. The mine is the sole outlier; the literal and the constant were introduced in the same commit.

One correction to the obvious diagnosis: it is *not* literally "never". `g2RingPollVitalForLogging()` at [System_SensorLogging.cpp:551](components/hardwareone/System_SensorLogging.cpp:551) has its own round-robin cursor that reaches deviceStatus roughly every 5th sample, and it fires immediately *before* the snapshot is taken — so its reply always lands after the row is written. Battery and wear are at best one mine stale and typically ~75 min stale at the 900 s default.

**Why it matters.** `r1_battery` and `r1_wear` are already unconditional CSV columns. They are blank or three-quarters-of-an-hour old in every Health Track capture you have. And `SYSEVT_RING_WORN` / `SYSEVT_RING_NOT_WORN` — selectable as automation triggers in three UIs — have effectively never fired headless, because nothing polled the query that produces them.

**The change.**

```diff
-      if (sHtMineCursor >= 4) {
+      if (sHtMineCursor >= G2_RING_POLL_VITAL_COUNT) {
```

Second half: `G2_RING_POLL_VITAL_COUNT` is defined **twice** in [G2_Ring.h](components/hardwareone/G2_Ring.h) — line 145 inside the `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`, line 220 inside the matching `#else`. The branches are mutually exclusive so they can't disagree in one translation unit, but they're two hand-maintained literals that must be bumped together and the stub copy has no consumer that would catch drift. Worse, line 145 was wedged between the `G2RingTelemetry` doc comment and the struct, orphaning the comment. Hoist one definition above the `#if`:

```cpp
// Poll cursor count for a full vitals refresh (HR→HRV→SpO2→Temp→deviceStatus).
// Lives outside the build-flag branches so enabled and stub builds can never drift.
static constexpr uint8_t G2_RING_POLL_VITAL_COUNT = 5;

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
```

…then delete the copy at 145 (restoring the telemetry doc comment to its struct) and the copy at 220.

Plus four string/comment corrections: the mine's state comment at :1194 ("polls all four vitals"), the two prose blocks in [System_SensorLogging.h:143-149](components/hardwareone/System_SensorLogging.h:143), the `healthstatus` help at :1697-1700 ("4-vital burst"), and the SUCCESS string at :1287 — which already reads `(HR/HRV/SpO2/temp/battery)`, so it only needs `+wear` appended.

**Other files that must change.**
- [G2_Ring.h](components/hardwareone/G2_Ring.h) — hoist the constant, delete both branch copies.
- [System_SensorLogging.h](components/hardwareone/System_SensorLogging.h) — two stale doc comments claiming a 4-vital burst.
- [docs/COMMAND_REFERENCE.md](docs/COMMAND_REFERENCE.md) — generated; regenerate with `python3 tools/command_registry.py reference` after the help edits. Do not hand-edit.

**Downstream.**

| What | Effect |
|---|---|
| CSV header at [:916](components/hardwareone/System_SensorLogging.cpp:916) + row builder at [:340](components/hardwareone/System_SensorLogging.cpp:340) | **benign** — unchanged. `r1_battery`/`r1_wear` are already emitted unconditionally; the fix fills columns, it doesn't add or reorder any |
| R1 change-dedup at [:588](components/hardwareone/System_SensorLogging.cpp:588) | **benefit** — battery drift and wear toggles now correctly un-suppress rows. Mine samples already pass `bypassR1Dedup=true`, so nothing regresses |
| `ringNoteWear` → `systemEventPost` at [G2_Ring.cpp:293](components/hardwareone/G2_Ring.cpp:293) | **benefit** — `SYSEVT_RING_WORN`/`NOT_WORN` start firing from the headless mine. Edge-gated by `sRingWearPosted`, so one event per real transition |
| [WebPage_Automations.h:481](components/hardwareone/WebPage_Automations.h:481) "R1 ring on/off finger" triggers + OLED create wizard | **benefit** — these have been selectable in three UIs while being unfireable headless. This is the user-facing headline; give it a changelog line |
| events.log file sink ([System_Debug.cpp:2302](components/hardwareone/System_Debug.cpp:2302), `eventLog` default ON) | **needs update** (awareness) — every wear transition now also writes a persisted line, subject to that log's rotation cap. Low volume, but it's a new writer to a persistent file |
| `notifRuleFor` overlay ([System_Notifications.cpp:113](components/hardwareone/System_Notifications.cpp:113)) | **benign today** — ring events default to `NSINK_NONE`. But a user who ran `notifydevicekind ring_worn <level>` will start getting banners from a headless device. Worth one changelog sentence |
| `g2HealthNoteSample(HEALTH_METRIC_BATTERY, …)` | **benefit** — battery sparkline gets one sample per mine instead of one per five. In-RAM ring, no flash cost |
| Mine period | **benign** — burst grows 3600→4300 ms; 1.2% against the 60 s floor, 0.08% against the 900 s default. Nothing derives a schedule from burst length |
| `g2RingPollVitalForLogging()` at :551 | **benign** — pre-existing redundancy, now a duplicate TX on 1-in-5 mines. `ringWrite`'s coalesce key collapses it. Out of scope |

**Risk.** Low. One extra 700 ms step and one extra fire-and-forget write per mine. `g2RingPollVital` already returns false when disconnected, `healthTrackTick` already bails to `HT_MINE_IDLE` on link loss mid-burst, and the deviceStatus frame is proven traffic that OLED, lens and the logger all already send. No new allocation, no stack growth, no task. The one thing a user could notice is battery/wear columns going from blank to populated mid-file if they keep an old capture running across the flash — acceptable per repo policy.

**How you'd know it worked.** `ringconnect`, `healthtrack interval 60`, `healthtrack on`, with the G2 Health page and OLED R1 Health screen **closed** (the mine yields to `g2HealthPageIsActive()`). With the G2 debug family on, count **five** `[RING] page poll: TX` lines per mine ending in `devStatus` — pre-fix you get four ending at `tempPoint`. Then `healthstatus` immediately after a mine: Bat and Wear must show ages under ~5 s, not `--` and not tens of minutes. Take the ring off and back on with only Track running: exactly one `SYSEVT_RING_NOT_WORN` and one `SYSEVT_RING_WORN`, not one per mine. Finally, **build the `ENABLE_G2_GLASSES=0` cell** — the hoisted constant has to compile in the stub branch, and a green FeatherS3 proves nothing about board-gated code.

**Size.** Minutes.

---

## Fix 4 — reconnect leaves a permanent hole in the sparklines

**What's broken.** Three coupled things, and fixing only the obvious one makes the graph visibly wrong.

1. Nothing arms a daily-history query on reconnect. Daily requests only originate from a lens tap or Trends Refresh.
2. Both the request gate ([G2_Health.cpp:352](components/hardwareone/G2_Health.cpp:352)) and the apply gate (:194) are `count >= kThinHistory` (8). After eight live samples the door is shut for the rest of the boot — catch-up is impossible by construction.
3. **The merge is an append, and the sparkline plots by array index, not time.** `drawSparkline` computes X as `x + 1 + (w-3)*i/(n-1)` and `seriesGetOrdered` walks insertion order; `ms` and `ringTs` are never read by the renderer. So the comment at :204 ("Synthesise millis timestamps … so the sparkline X axis has monotonic order") is factually wrong — today's daily history is drawn *to the right of* the live samples, reading as if it happened later.

Two more latent defects in the same function: when `span == 0` every synthesised `rts` collapses to `startTs`, so `seriesPush`'s ring-ts dedupe drops all but the first value; and appending up to 64 daily samples into a 96-deep ring evicts the live trail out the back.

**Why it matters.** Every BLE drop punches a hole in the health graphs that never fills, and the one code path meant to fill it draws the fill in the wrong place.

**The change.** Three parts.

*(a) Prepend, never append.* New helper in [G2_Health.cpp](components/hardwareone/G2_Health.cpp) after `seriesPushRaw`:

```cpp
// Backfill MUST land before the live trail: seriesGetOrdered / drawSparkline walk
// insertion order and ignore ms/ringTs entirely. Only fills unused capacity —
// never evicts a live sample — and leaves last*/head alone, since those describe
// the newest end, which prepending doesn't move.
static void seriesPrepend(HealthSeries* s, int16_t value, uint32_t ringTs, uint32_t ms) {
  if (!s || s->count >= HEALTH_HIST_CAP) return;
  const size_t oldest = (s->head + HEALTH_HIST_CAP - s->count) % HEALTH_HIST_CAP;
  const size_t slot   = (oldest + HEALTH_HIST_CAP - 1) % HEALTH_HIST_CAP;
  s->buf[slot].ms = ms; s->buf[slot].value = value; s->buf[slot].ringTs = ringTs;
  s->count++;
}
```

`g2HealthApplyDailyBackfill` then drops its count gate entirely (the merge itself is the protection), computes a `cutoff = seriesOldestRingTs(s)` so it only fills the region *before* the oldest datable live sample, walks newest→oldest so each prepend lands in front of the previous one, and skips `values[idx] == 0` (daily buckets read 0 for "not worn"; feeding zeros in drags the auto-scaled Y axis to the floor).

*(b) Time-based gate instead of count-based.*

```diff
-  if (!s || s->count >= kThinHistory) return;
+  if (!seriesWantsDaily(s)) return;
   if (!g2RingIsConnected()) return;
+  seriesNoteDailyRequested(s);
```

with `HealthSeries` gaining a `uint32_t lastDailyMs` and:

```cpp
static bool seriesWantsDaily(const HealthSeries* s) {
  if (!s) return false;
  const uint32_t now = millis();
  if (s->lastDailyMs != 0 &&
      (long)(now - s->lastDailyMs) < (long)kDailyCooldownMs) return false;
  if (s->lastMs == 0) return s->count < kThinHistory;   // ← never saw a live sample;
                                                        //   not "infinitely stale"
  if (s->count < kThinHistory) return true;
  if (s->count >= HEALTH_HIST_CAP) return false;        // no room to backfill into
  return (long)(now - s->lastMs) >= (long)kStaleGapMs;
}
```

That `lastMs == 0` line is a **correction** to the original plan. `seriesPrepend` correctly doesn't touch `lastMs`, so a series populated only by backfill keeps `lastMs == 0` and would read as infinitely stale for the whole boot — which is the normal state of any metric the ring never point-reports (temp on rings that don't support it).

*(c) Arm on link-up, drain from the main loop.* `g2HealthNoteRingLinkUp()` at the end of `ringRunStandardSetup()` sets a due-time; `g2HealthCatchupTick()` (called from `healthTrackTick`, above the `healthTrackingEnabled` early-out) waits out a **20 s** settle (`kCatchupSettleMs = 20000`), then queues one daily per stale metric. Two corrections here:

> **Settle revised 8 s → 20 s (2026-07-29).** The original 8 s was justified as "avoid bulk traffic in the reconnect window". That justification was wrong — `docs/R1_RING_DISCONNECT_INVESTIGATION_2026-07-28.md` calls the drops ring-side and statistically refutes the naive replay model (Pearson −0.14, n=18). 8 s is nonetheless *actively harmful* against the ring-clock custody work: `everyMs` fires immediately on a zero stamp, so the dark probes land at roughly setup+0/+5/+10 s, and an 8 s settle drops the first daily between probes 2 and 3 with its multi-record reply arriving 13–15 s after link-up — inside the 14.0 s ±36 ms window that killed 4 of 19 sessions. 20 s clears the probe ladder and that window. The settle survives for its *other* reasons: stay off the connect worker's tail, and don't contend with the custody tick.

```cpp
static volatile uint32_t sCatchupDueMs = 0;   // written by the BLE connect worker,
                                              // read/cleared by the main loop
```

and reset the queue before sweeping — `queueDailyCmd`'s dedup only scans `idx..len` and the queue self-resets **only on a full drain** ([G2_Health.cpp:519-527](components/hardwareone/G2_Health.cpp:519)), so a Trends page closed mid-drain leaves `idx=1,len=3` forever, blocking the sweep and later feeding stale queries to the new pump:

```cpp
  sCatchupDueMs = 0;
  if (!g2RingIsConnected()) return;
  sDailyQueueLen = 0; sDailyQueueIdx = 0;   // stale partial drain from a closed page
```

The off-page pump in [System_SensorLogging.cpp](components/hardwareone/System_SensorLogging.cpp), inserted after the `HT_OD_SETTLE` block, paced at 2000 ms (daily replies are large multi-record payloads, not point queries) and mutually excluded from the lens worker by `g2HealthPageIsActive()` — the same soft guard the on-demand burst already uses.

**Other files that must change.**
- [G2_Health.h](components/hardwareone/G2_Health.h) — declare `g2HealthNoteRingLinkUp` / `g2HealthCatchupTick`, plus inline no-op stubs in the `!ENABLE_R1_HEALTH` block.
- [G2_Ring.cpp](components/hardwareone/G2_Ring.cpp) — one call at the end of `ringRunStandardSetup()`.
- [System_SensorLogging.cpp](components/hardwareone/System_SensorLogging.cpp) — the pump and its `sHtDailyLastMs` pacer.

**Downstream.**

| What | Effect |
|---|---|
| `drawSparkline` / `seriesGetOrdered` | **benefit** — this is what makes catch-up correct. Re-gating alone produces a visibly wrong graph |
| `g2HealthApplyTrendDaily` on the same response | **benefit** — one daily response feeds both Trends and the live backfill. Today a Trends Refresh silently clobbers a thin live series; prepend resolves that rather than deepening it |
| `sDailyQueue[4]` at [G2_Health.cpp:62](components/hardwareone/G2_Health.cpp:62) | **breaks if extended** — the sweep queues exactly 4, filling the array. `queueDailyCmd` silently returns when full. Grow to 8 before anything adds a 5th row |
| `g2HealthOpen` clearing the queue | **benign** — opening the page mid-drain discards queued dailies and the tick disarms. The page issues its own; the cooldown stamp prevents an immediate re-ask |
| Lens daily drain at [G2_Glasses.cpp:17485](components/hardwareone/G2_Glasses.cpp:17485) | **benign** — unchanged API; mutual exclusion is `g2HealthPageIsActive()` |
| `const bool fetching = (sDailyQueueIdx < sDailyQueueLen)` at [G2_Health.cpp:1038](components/hardwareone/G2_Health.cpp:1038) | **benign** — the new pump mutates `sDailyQueueIdx` from the main loop, so this indicator can flicker if the page opens in the guard window. Cosmetic |
| Ring link stability | **needs watching** — see Risk. This is the one real hazard |
| OLED / web `/r1-health` / `healthstatus` JSON | **benign** — grep-verified: none touch `HealthSeries` or the daily path. No schema, command or setting changes |
| [CHANGELOG.md:15](CHANGELOG.md:15) "optional daily-history backfill when a metric is focused" | **needs update** — now inaccurate; backfill also runs on reconnect |
| [docs/USERGUIDE.md:212](docs/USERGUIDE.md:212), :1304 | **needs update** — one sentence that gaps now fill automatically |

**Risk.** The biggest risk is link stability, not memory. This injects up to 4 write-no-response frames plus 4 large multi-record notify replies into a link whose observed deaths cluster at 14.0 s. Mitigated by the **20 s** settle, 2 s spacing and stale-only selection (a short blip queues nothing). **If hardware testing shows reconnect drops get worse, the remaining knobs in order are: widen the pump 2000→5000 ms, then cut the sweep to HR only.** Do not "fix" it by moving the sweep into `ringRunStandardSetup`'s inline delay chain — that puts it at t+0 on a virgin link and stalls the shared BLE-connect worker.

Also gate the drain on more than the timer: `!g2RingConnectInFlight()` (the arm runs *on* the connect worker while `gRingConnectTaskActive` is still true), on `g2RingRxPacketCount()` having moved since arm, and on `!g2HealthPageIsActive()`.

Memory: zero heap, zero tasks, no PSRAM. `HealthSeries` grows 4 bytes × 8 instances = 32 B `.bss`. `seriesPrepend` is O(1) and the scan helpers are O(count≤96) with no temporaries — deliberately chosen over a rebuild-into-scratch merge, because `g2HealthApplyDailyBackfill` runs on the BLE notify path (BTC_TASK, 3–4 KB) where a 1152-byte `HealthSample[96]` temp would be reckless.

Behavioural: `kDailyCooldownMs` is the only thing between an idle-but-connected ring and a daily query every 10 min per metric forever. If that's too chatty in practice, raise the cooldown — do not remove the staleness test, or the original bug returns.

**How you'd know it worked.** The regression this exists to prove: let the ring run 10+ minutes so each series holds well over 8 live samples, then `ringdisconnect` / `ringconnect`. The catch-up **must** queue — before this change it queued nothing. Look for `[HEALTH] ring link up — daily catch-up armed in 20000 ms`, then `queued 4 daily queries`, then four backfill lines with `added > 0`. Open Apps → Health → Heart Rate: the sparkline must read left-to-right oldest→newest with no discontinuity where backfill meets live data (the append bug showed as the day curve tacked onto the right). Note `n=` before and after a Trends Refresh — it must never go down. Then the one that decides whether it ships: leave it connected 30+ minutes across several reconnects and compare `rsn=0x8` time-to-first-drop against a build without the sweep, using Fix 5's enriched drop line.

**Size.** A session.

---

## Fix 2 — the log throws away freshness, and health.csv isn't CSV

**What's broken.** Two defects that are pointless to fix separately.

*(1) Freshness.* `sensorLogTick`'s R1 snapshot at [:552-566](components/hardwareone/System_SensorLogging.cpp:552) copies `G2RingTelemetry`'s values and drops all six `*AgeSec` fields. The R1 point queries return whatever the ring last auto-recorded — [G2_Ring.cpp:270](components/hardwareone/G2_Ring.cpp:270) says so explicitly ("the ring's auto-recording cadence updates them every few minutes, not on demand") — so a fresh notify reply and a ten-minute-old cache re-read produce **byte-identical rows**. The information already exists: `ringSampleAgeSec` computes it, `G2RingTelemetry` carries it, `buildHealthStatusJson` publishes it, and the web UI renders it as "12s ago". Only the durable log — the artifact you actually analyse later — throws it away.

*(2) Format.* `healthTrackSet` coerces only `TRACK → CSV`, and `gSensorLogFormat` defaults to `SENSOR_LOG_TEXT`. So a device that has never run `sensorlog format csv` writes TEXT into a file named `.csv`. Every capture in `docs/HealthCapture/` proves it — line 1 is `[2026-07-25 22:49:52.179] | r1: hr=95 hrv=46 spo2=99 bat=88`. The header block never runs, so the carefully-ordered `r1_*` column names have **never been written to disk**.

**Why it matters.** Adding age columns without the format fix is dead code on the health path. Fixing the format without the ages gives you a well-formed CSV that still can't distinguish a fresh sample from a stale one. And shipping them separately changes the file shape twice.

**The change.**

*Ages.* Six `int32_t r1*AgeSec` fields on `SensorCacheSnapshot`, filled from the telemetry copy, rendered through one shared formatter:

```cpp
// wrap=true renders "(12s)" against a TEXT value; wrap=false a bare "12" for a CSV
// cell. Invalid vitals and unknown ages render empty so both formats stay aligned.
static void r1AgeToken(char* out, size_t cap, bool valid, int32_t ageSec, bool wrap) {
  if (!out || cap == 0) return;
  if (!valid || ageSec < 0) { out[0] = '\0'; return; }
  if (wrap) snprintf(out, cap, "(%lds)", (long)ageSec);
  else      snprintf(out, cap, "%ld", (long)ageSec);
}
```

TEXT output becomes `r1: hr=68(41s) hrv=57(41s) spo2=98(41s) temp=-- bat=84(2s) wear=on(2s)` — the same parenthesised-qualifier idiom the ToF fields already use. CSV interleaves an age column after each vital, and the header must match exactly:

```cpp
        if (gSensorLogMask & LOG_R1) csvHeader += ",r1_connected"
                                                  ",r1_hr,r1_hr_age_s"
                                                  ",r1_hrv,r1_hrv_age_s"
                                                  ",r1_spo2,r1_spo2_age_s"
                                                  ",r1_temp,r1_temp_age_s"
                                                  ",r1_battery,r1_battery_age_s"
                                                  ",r1_wear,r1_wear_age_s";
```

**Correction to the original plan:** the header comment says "−1 = unknown", but [:412](components/hardwareone/System_SensorLogging.cpp:412) declares `SensorCacheSnapshot snap = {};` which value-initialises to **0**, and the fill block sits inside `#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES`. On a board with G2 compiled out the fields keep 0, which `r1AgeToken` would render as "sampled this instant". Not reachable today (the `r1*Valid` flags are filled in the same guarded block), but the comment and the initialiser disagree, which is exactly what a later refactor walks into. Set the six fields to −1 explicitly next to the `= {}`.

Design note on why age and not the ring's absolute epoch: `G2RingTelemetry` already carries `*AgeSec` and nothing else; the raw `hrTs` is file-static in G2_Ring.cpp; the ring's epoch is only as good as the `systemTime` we pushed at pair time and `ringSampleAgeSec` silently falls back to millis-since-notify when the sanity gate fails, so a raw `ts` column would be ambiguous between two clocks with no marker. `row_time − age` recovers the sample time in the same frame as every co-logged sensor. And battery/wear have no ring timestamp at all, so a `ts` column would be permanently empty for two of six.

*Format coercion — with the correction.* The naive widening is broken. `healthTrackSet` has an "already logging" early return at [:1486](components/hardwareone/System_SensorLogging.cpp:1486) that sits **after** the coercion and returns **without** calling `healthTrackRestartWithCurrentMask()`:

```cpp
    if (gSensorLoggingRunning && (gSensorLogMask & LOG_R1)) {
      // Already logging with R1 in the mask — nothing more to do.
      ...
      return getDebugBuffer();
    }
```

Today that's latent (only TRACK triggers coercion). Widen the coercion to `!= SENSOR_LOG_CSV` and it becomes the default path: run `sensorlog sensors r1` + `sensorlog start` in TEXT, then `healthtrack on`, and the format flips on an **already-open file** with no restart, so the header block never runs and CSV rows get appended after TEXT rows with no header at all. So:

```cpp
    // The health capture is a .csv and the analysis path wants fixed columns, so
    // force CSV — not just when the current format is the GPS-only TRACK.
    const bool formatChanged = (gSensorLogFormat != SENSOR_LOG_CSV);
    if (formatChanged) {
      gSensorLogFormat = SENSOR_LOG_CSV;
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_CSV);
      if (isDebugFlagSet(DEBUG_LOGGER)) DEBUG_LOGGERF("healthtrack: forced sensorlog format to CSV");
    }
```

and then gate the early return so a format change always forces the restart:

```diff
-    if (gSensorLoggingRunning && (gSensorLogMask & LOG_R1)) {
+    if (gSensorLoggingRunning && (gSensorLogMask & LOG_R1) && !formatChanged) {
```

Same treatment in `sensorLogAutoStart` — and **persist it**, unlike the original prescription:

```diff
-    if (gSensorLogFormat == SENSOR_LOG_TRACK) gSensorLogFormat = SENSOR_LOG_CSV;
+    if (gSensorLogFormat != SENSOR_LOG_CSV) {
+      gSensorLogFormat = SENSOR_LOG_CSV;
+      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_CSV);
+    }
```

(The value does eventually reach settings.json via `cmd_sensorlog`'s start path at [:974](components/hardwareone/System_SensorLogging.cpp:974), but relying on that leaves the runtime and the settings tree disagreeing for the intervening window and makes the persistence invisible at the call site.)

*Buffer arithmetic.* TEXT: the widest age token is `(2147483647s)` = 13 chars, so `char[16]` is safe. The R1 segment grows ~60→~138 B. The all-sensors worst case was **already** ~532 B against a 512-byte buffer — it truncates today, silently but safely, because `remaining` goes non-positive and every later block is gated on `remaining > 0`. Bump to 768 (one-time PSRAM alloc). CSV grows +66 B worst case against a 1024-byte buffer — untouched.

**Other files that must change.**
- [System_SensorLogging.h](components/hardwareone/System_SensorLogging.h) — the six snapshot fields; the `healthTrackSet` doc comment at :128 already claims CSV and only becomes true with this fix.
- [docs/COMMAND_REFERENCE.md](docs/COMMAND_REFERENCE.md) — regenerate; `healthtrack` and `healthlogmerge` help strings change.
- [docs/USERGUIDE.md:1081-1092](docs/USERGUIDE.md:1081) — state that Track forces CSV, list the `r1_*` columns, and say an empty age cell means "no sample / unknown", never "zero seconds old".
- [docs/QUICKSTART.md:51](docs/QUICKSTART.md:51) — "`healthtrack on` starts background vitals logging" now produces something materially different.

**Downstream.**

| What | Effect |
|---|---|
| CSV header at [:916](components/hardwareone/System_SensorLogging.cpp:916) | **breaks** — must gain the six `*_age_s` names in the exact interleaved order, or every column from `r1_hr` on is mislabelled by one position |
| `sensorLogAutoStart` coercion at [:1762](components/hardwareone/System_SensorLogging.cpp:1762) | **breaks** — fix only `healthTrackSet` and a reboot resumes in TEXT. Both sites change together |
| `healthTrackSet` early return at [:1486](components/hardwareone/System_SensorLogging.cpp:1486) | **breaks** — see above. This is the correction that makes the fix actually work |
| `docs/HealthCapture/*.csv` — 14 existing captures | **breaks** — new captures change delimiter *and* column set. No back-compat per repo policy; rename the dir or note the cutover. Do **not** add migration code |
| CSV header written only inside `if (!VFS::existsGuarded(filepath))` at [:885](components/hardwareone/System_SensorLogging.cpp:885) | **breaks (product decision)** — `healthTrackSet` uses `CAPTURE_HEALTHLOG_DEFAULT` verbatim; only `sensorLogAutoStart` timestamps. So repeated `healthtrack on/off/on` appends to one `health.csv` with **one** header. Change the mask between toggles and the columns change with no new header. Decide: accept it, or timestamp the healthtrack path too |
| [WebPage_Logging.h:711](components/hardwareone/WebPage_Logging.h:711) — Apply Config sends `sensorlog format <sel>` | **breaks (silently)** — a THIRD surface that un-coerces a running Track session back to TEXT. Anyone who opens /logging and clicks Apply while Track runs reverts health.csv to pipe-delimited rows |
| `sensorlogformat` per-setting command ([System_Settings.cpp:2788](components/hardwareone/System_Settings.cpp:2788)) | **breaks (silently)** — a FOURTH surface, reachable from CLI, web Settings, OLED and the lens. Needs a guard or a `cliHint` when `healthTrackingEnabled` |
| OLED Logging format cycler ([OLED_Mode_Logging.cpp:271](components/hardwareone/OLED_Mode_Logging.cpp:271)) | **benign-ish** — can move an active session off CSV mid-run; re-forced on the next toggle or reboot. Unchanged from today's TRACK behaviour, but now reachable by default |
| `healthtrack off` path ([:1506](components/hardwareone/System_SensorLogging.cpp:1506)) | **needs decision** — the coercion is never undone. A user logging GPS/IMU in TEXT who toggles Track on then off is left permanently on CSV |
| Thermal/ToF row gates at [:280](components/hardwareone/System_SensorLogging.cpp:280)/:287 | **breaks (pre-existing)** — the header is built purely from the mask but those row blocks are additionally gated on `s.thermalValid`/`s.tofValid`, so a mixed-sensor row can carry fewer columns than the header. Name it; this fix is what makes anyone read that header for real |
| R1 change-dedup at [:588](components/hardwareone/System_SensorLogging.cpp:588) | **benign** — age must **not** join the comparison key; it ticks every sample and would suppress nothing |
| `buildHealthStatusJson` / `/api/health/status` / `/r1-health` "Ns ago" | **benefit** — unchanged; they already publish ages. The log now agrees with what the web page has shown all along. That's the point |
| [System_Maps.cpp:2770](components/hardwareone/System_Maps.cpp:2770) sensor-log track parser | **benign** — an R1-only CSV has no `gps:` marker, so it would read `r1_connected` as lat. Pre-existing hazard, only reachable if someone loads health.csv as a map track. Follow-up guard, not a blocker |
| [G2_Page_Files.cpp:388](components/hardwareone/G2_Page_Files.cpp:388) lens TextPager `.csv` view | **benign** — the header grows from 7 to 13 r1 columns and soft-wraps across many lens lines. Cosmetic, but it's the surface a user checks first |
| Any parser, anywhere | **benign** — grepped the whole tree (tools/*.py, all .cpp/.h, web JS, docs): nothing reads health.csv or sensorlog output except the Maps track parser. No Android/python consumer, no chart code |

**Risk.** The format flip is user-visible: turning Health Track on now overrides an explicit `sensorlog format text`. Intended, and it matches the existing TRACK override, but it deserves a changelog line — especially since four surfaces can undo it. Age basis can switch mid-session (`ringSampleAgeSec` prefers the ring epoch once both clocks pass the ≥1600000000 gate, so an age can step when NTP lands); that's inherent to the existing helper and already visible on the web page — do not "fix" it in the logger. Battery and wear always report age from local receive, so they will look systematically fresher than HR/HRV/SpO2/temp; document it in the header comment so an analyst doesn't read it as the ring sampling battery more often. No stack impact — both builders use static PSRAM buffers plus ~96 B of new locals on the main-loop task.

**How you'd know it worked.** Erase flash first (a stale TEXT health.csv gives you a mixed file). `sensorlog format text`, then `healthtrack on` — `sensorlog status` must now report `Format: CSV`. **Correction to the obvious test step:** look at `/logging_captures/sensors/health.csv`, *not* `health-<ts>.csv` — only the boot-resume path timestamps. Line 1 must be the header ending `…,r1_wear,r1_wear_age_s`; count commas in a data row against the header (on an R1-only capture — on a mixed one you'll hit the pre-existing thermal/ToF gate). Then the discrimination test that is the whole point: note the newest row, wait past the poll interval with no mine, then `healthstatus poll`. The forced row must show small ages for vitals the ring actually re-sampled and visibly larger ages for those it didn't — before this change those rows were indistinguishable. Confirm `r1_temp` and `r1_temp_age_s` are both *empty* (not `-1`, not `0`) on a ring that never reports temp. Reboot with Track on and confirm `Format: CSV` survives. Build a board with G2 disabled.

**Size.** Under an hour for the code; the four un-coercion surfaces are what turn it into a session if you decide to guard them.

---

## Fix 3 — steps and kcal are decoded, printed to a log line, and dropped

**What's broken.** Two independent drop sites plus a missing on-ramp.

*(1) RX dispatch.* `g2RingQueryDaily()` accepts and transmits `R1_CMD_ACTIVITY`, but the cache extractor at [G2_Ring.cpp:468](components/hardwareone/G2_Ring.cpp:468) matches `R1_SUB_DAILY` only for `HEARTRATE|HRV|SPO2|TEMPERATURE`. An activity reply falls past that block, past the deviceStatus block (module mismatch), past wearStatus, and returns having stored nothing. The only thing that touches those bytes is `annotateActivityDaily()`, which formats slot/steps/kcal into a `char abuf[256]`, logs it, and lets the buffer go out of scope. **Steps and kcal are decoded correctly and then thrown away.**

*(2) Forwarded path.* In the G2-forwarded `RingRawData` path, [G2_Ring.cpp:630](components/hardwareone/G2_Ring.cpp:630) ends its field switch with `default: break;  // chargeStates / kcal / steps not cached today`. Proto fields 11 (actKcal) and 15 (steps) are read off the wire as varints and discarded. Different producer, different envelope — needs its own two `case` arms.

*(3) Nothing asks.* No surface ever requests activity/daily except the manual `ringquery activity daily` CLI. Fixing the dispatch alone leaves every surface showing `--` forever.

**Why it matters.** Two real metrics the hardware already sends, invisible on all four surfaces and absent from every capture.

**The change.**

*Parser.* A real `r1ParseActivityDaily()` in [System_R1_Protocol.cpp](components/hardwareone/System_R1_Protocol.cpp), with the record layout extracted into one shared `readActivityRecord()` that both the annotator and the parser use, so the wire layout lives in exactly one place. Bounds are derived, not guessed: `R1_MAX_PAYLOAD` is 256, so `R1_ACTIVITY_MAX_RECORDS = (256-7)/7 = 35` and a full frame can never overflow the array. Add a self-test vector to `r1ProtocolSelfTest()` over the existing `pActivity` capture — count=5, partial=2, steps 0+7+0+8+97=112, kcal 2+20+14+16+23=75. That runs at ring init, before any BLE traffic.

**Stack verdict, and it drove the design.** `ringDumpFrame()` runs on Bluedroid's BTC_TASK. `sdkconfig` has `BT_BTC_TASK_STACK_SIZE=8192`, but `sdkconfig.esp32s3` (board default) is **4096**, and the frame already holds `R1Decoded d` (~292 B) + `char pbuf[196]` + `char abuf[256]` + `char buf[196]` ≈ 950 B before the extractor is even called. So the ~292 B parse result lives in file-static PSRAM BSS:

```cpp
// ~292 B — more than the BLE notify task can spare. Bluedroid dispatches every
// GATT-client notify on one task, so ringDumpFrame is never reentered and one
// static instance is safe. Do NOT "simplify" this to a local.
EXT_RAM_BSS_ATTR static R1ActivityDailyResult gR1ActScratch;
```

*Day accumulator.* Activity/daily arrives paginated. Summing per frame double-counts on any re-query; a high-water slot cursor undercounts, because the ring revises the *current* bin upward as you keep walking. Keeping the bins makes both idempotent — a record overwrites its slot and the totals re-sum:

```cpp
static constexpr uint8_t R1_ACT_SLOTS_PER_DAY = 144;   // 24 h at 10 min
struct R1ActivityDay { uint32_t baseTs; uint16_t steps[144]; uint16_t kcal[144]; };
EXT_RAM_BSS_ATTR static R1ActivityDay gR1ActDay;       // 576 B, PSRAM BSS
```

`ringApplyActivityDaily()` parses, rejects a `baseTs` below the same 1600000000 sanity floor `ringSampleAgeSec` uses, wipes the bins on a day change, writes each slot, re-sums, and stamps `gR1Cache.steps/kcal`. **Correction:** log the rejects. The original left both early returns silent, which means a ring whose clock never took our `systemTime` produces permanently-invalid steps with no explanation — indistinguishable from a wire-format change:

```cpp
  if (gR1ActScratch.baseTs < 1600000000u) {
    DEBUG_G2F("[RING] activity daily: ignoring frame, baseTs=%lu below sanity floor "
              "(ring clock not synced?)", (unsigned long)gR1ActScratch.baseTs);
    return;
  }
```

*Drop site 1* — a new branch after :484, its own rather than widening the cmd test above, because the record shape is completely different:

```cpp
  if (d.module == R1_MODULE_HEALTH && d.subCmd == R1_SUB_DAILY &&
      d.cmd == R1_CMD_ACTIVITY) {
    ringApplyActivityDaily(d);
    return;
  }
```

*Drop site 2* — replace the `default: break;` with cases 11 and 15, range-checked. Cache `actKcal` (11), not `allKcal` (13): the daily-bin sum is an activity figure, and mixing the two definitions in one field makes the number jump when the source switches.

*On-ramp — and this is the one place I am overriding the original plan.* The plan added `case 5: activity daily` to `g2RingPollVital` and bumped `G2_RING_POLL_VITAL_COUNT` to 6. **Don't.** That function's documented contract is "send ONE vitals point-query", and five drivers loop the whole ladder assuming small request / small reply:

- [OLED_Mode_R1_Health.cpp:74-76](components/hardwareone/OLED_Mode_R1_Health.cpp:74) — fires the burst on page entry **and re-arms `sHealthPollCursor = 0` on every reconnect** (`if (conn && !sHealthWasConn)`), at 800 ms spacing;
- the lens Health page entry and every Poll Now;
- `healthstatus poll` / web Poll Now;
- the Track mine;
- and the unbounded one: `g2RingPollVitalForLogging()`, which `sensorLogTick` calls on **every sample**, gated only by an internal 700 ms. At a 5 s log interval and COUNT=6, that fires a multi-frame paginated activity query roughly every 30 s for the life of any mixed sensorlog session.

That directly contradicts Fix 4, which engineers a 20 s settle and 2 s spacing to keep the link-up window quiet. The two designs cannot both be right about how much daily traffic the link tolerates.

So: leave `G2_RING_POLL_VITAL_COUNT` at 5 and keep `g2RingPollVital` point-only. Route activity/daily through the machinery Fix 4 already builds — `queueDailyCmd(R1_CMD_ACTIVITY)` from the catch-up sweep, drained via `g2HealthConsumeDailyRequest` → `g2RingQueryDaily`, which already validates `R1_CMD_ACTIVITY` and uses a distinct coalesce key (`0x20+cmd = 0x25`). It inherits the 2000 ms pacing and 20 s settle for free. **`sDailyQueue` must grow from 4 to 8 in this same change** — the sweep already fills it exactly and `queueDailyCmd` drops the overflow silently.

*Cache and surfaces.* `gR1Cache` gains steps/kcal + rxMs + valid; `G2RingTelemetry` gains steps/kcal/valid + `stepsAgeSec`/`kcalAgeSec`, computed as `ringSampleAgeSec(0, rxMs)` (local receive, like battery/wear — day totals carry no ring per-sample epoch). **Both copies** of the struct in [G2_Ring.h](components/hardwareone/G2_Ring.h) — real and stub — must move in lockstep.

Do **not** clear `gR1ActDay` on disconnect. Today's bins are still today's bins across a reconnect; wiping them makes the totals dip to 0 on every drop. The `baseTs` check handles the day roll on its own.

**Other files that must change.**
- [System_R1_Protocol.h](components/hardwareone/System_R1_Protocol.h) / [.cpp](components/hardwareone/System_R1_Protocol.cpp) — the struct, the parser, the shared record reader, the self-test vector.
- [G2_Ring.h](components/hardwareone/G2_Ring.h) — telemetry struct, **both** branches.
- [System_SensorLogging.h](components/hardwareone/System_SensorLogging.h) + [.cpp](components/hardwareone/System_SensorLogging.cpp) — snapshot fields, TEXT builder, CSV builder + header, `buildHealthStatusJson` schema 2→3.
- [G2_Health.cpp](components/hardwareone/G2_Health.cpp) — `sDailyQueue[4]` → `[8]`; the Overview text line.
- [G2_Glasses.cpp](components/hardwareone/G2_Glasses.cpp) — the `readout` buffer bump (see below).
- [OLED_Mode_R1_Health.cpp](components/hardwareone/OLED_Mode_R1_Health.cpp) — the 128×64 relayout.
- [WebPage_R1_Health.cpp](components/hardwareone/WebPage_R1_Health.cpp) — two stat blocks + two `hw.setText` lines.

**Two citation corrections from cross-checking, because they'd send you to the wrong file:**
- `kOverviewTextGeom` is **not** in G2_Health.cpp. It's `const G2ContainerGeom kOverviewTextGeom = { 280, 8, 288, 272 };` at [G2_Glasses.cpp:17358](components/hardwareone/G2_Glasses.cpp:17358). The pane-fits reasoning still holds (6 lines today, 7 × ~28 px ≈ 196 px of 272).
- The `readout[160] → [208]` edit targets [G2_Glasses.cpp:17465-17466](components/hardwareone/G2_Glasses.cpp:17465). `char readout[160];` appears **five times** in that file (4583, 4956, 5251, 6123, 17465) and line 5251 is byte-identical in the FM tuner renderer. Anchor on the adjacent `char lastReadout[160];` — that one is unique.

**Downstream.**

| What | Effect |
|---|---|
| `G2RingTelemetry` + all 13 call sites | **benign** — struct grows ~20 B (≈48→68), copied by value onto normal task stacks. New fields appended; existing reads unaffected |
| G2_Ring.h stub branch | **breaks** — stub struct must be edited in lockstep or a non-BLE board fails to compile. Board-gated code hides compile breaks; verify with a second config |
| `sDailyQueue[4]` at [G2_Health.cpp:62](components/hardwareone/G2_Health.cpp:62) | **breaks** — must become `[8]`. `queueDailyCmd` drops the 5th entry silently |
| CSV header + row builder + TEXT builder | **breaks** — append `,r1_steps,r1_steps_age_s,r1_kcal,r1_kcal_age_s` in Fix 2's interleaved style. The original plan omitted the ages, which would leave six aged vitals and two unaged in the same file |
| R1 change-dedup `flags` byte at [:592](components/hardwareone/System_SensorLogging.cpp:592) | **breaks** — steps/kcal must count as a change or an all-else-static row is suppressed. **And `flags` is a `uint8_t` already using bits 1,2,4,8,16,32** — steps and kcal fill 64 and 128 exactly, leaving zero headroom. Widen to `uint16_t` in the same edit |
| `buildHealthStatusJson` schema 2→3 | **needs update** — payload ~420→~515 B; `kBufSize=768` and the 1024 debug buffer both still fit. Note the `!ENABLE_R1_HEALTH` branch hard-codes `"schema":1` with a different shape — bump it or document that 1 is reserved for the disabled form |
| [G2_Glasses.cpp:17465](components/hardwareone/G2_Glasses.cpp:17465) `readout[160]` | **breaks** — worst case goes ~148→~179 B; `snprintf` would truncate the status line, which *also* defeats the `strcmp(readout, lastReadout)` UPDATE_TEXT gate. Bump both to `[208]` |
| [OLED_Mode_R1_Health.cpp](components/hardwareone/OLED_Mode_R1_Health.cpp) `displayR1Health` | **breaks** — the page is full. 128×64: header 10, footer 10, rows at y=11/20/29/38 (all six cells used) and 49/57. A fourth vital row pushes the actions off-screen. Fix without adding a row: move wear onto the title line ("R1 HEALTH [OK] on" = 17 of 21 chars) and make row 3 three cells — `T 36.5C` / `S8421` / `K320` |
| `freshestVitalAge()` at [OLED_Mode_R1_Health.cpp:52](components/hardwareone/OLED_Mode_R1_Health.cpp:52) | **needs decision** — this feeds the shared "recentness" label on both OLED and the lens Overview. Day totals refresh on a different cadence and would dominate the min(). Decide in or out, and comment it |
| `annotateActivityDaily` log line | **benign** — refactored onto the shared reader; output byte-identical. Self-test vector 7 still covers it, new vector 9 pins the numbers |
| `ringquery activity daily` help ([G2_Ring.cpp:1706](components/hardwareone/G2_Ring.cpp:1706)) | **needs update** — documented as a raw protocol probe; it now mutates a cache four surfaces render. Stale help is this repo's most recurring defect class |
| [docs/R1_RING_PROTOCOL.md](docs/R1_RING_PROTOCOL.md) §7.3, §15.6, capability table | **needs update** — the opcode goes from "decoded to a log string" to "cached as day totals". §7.3's record-layout block is the source this parser is derived from; cross-reference it |
| [README.md:45](README.md:45), :95 — "health vitals (heart rate, HRV, SpO2, temperature)" | **needs update** — the explicit four-item list becomes incomplete |
| [docs/APP_JSON_CONTRACT.md](docs/APP_JSON_CONTRACT.md) | **needs update (pre-existing gap)** — `healthstatus` / `/api/health/status` isn't in the contract doc at all, so schema has now moved twice with no entry |
| ESP-NOW `REMOTE_SENSOR_*`, MQTT publish, automation `evaluateCondition` | **benign** — grep-confirmed: there is no R1/health member in any of the three. Nothing breaks; nothing can consume the new data either |
| BLE spoof-push (`ringtoglasses`) | **benign** — could now forward real steps/kcal to the temples' native UI, but that command is already DEPRECATED and unregistered. Leave it |

**Risk.** RAM: +576 B + 292 B PSRAM BSS, +~40 B DRAM in `gR1Cache`, +20 B per stack copy of the telemetry struct. Neither static is a task stack and neither holds a secret. Reentrancy: the single static scratch is only safe because Bluedroid dispatches every GATT notify on one task; if a future change parses ring frames from a worker, it needs a guard.

Semantic mixing: the forwarded path writes `gR1Cache.steps` directly without touching `gR1ActDay`, so a device receiving both a forwarded push and its own activity/daily could see the total step backwards on the next daily frame. In practice they're mutually exclusive (either we own the ring link or the temple does) and both mean "today's total". Documented in the case-11/15 comment; don't try to reconcile them.

**RE confidence is the honest weak point.** The 7-byte header and `{slot, steps, ?, ?, kcal}` record layout are reverse-engineered from a **single 2026-05-02 capture** and explicitly do not match the python codec's 14-byte header. The 10-min-bin / 144-slot-day assumption is inferred from slot 54 → 09:00. If a ring firmware update changes the header length, the parser returns records that are garbage-but-in-range and the totals go wrong **silently**. Mitigation is the per-frame `[RING] activity daily:` line plus the raw hex dump above it, so a shape change shows up in one `ringquery activity daily`.

Day rollover: `baseTs` comes from the ring, whose clock is only as good as the `systemTime` we push at pairAuth (deliberately tz=0/UTC). "Today" is a UTC day; a user in UTC-8 sees the total reset at 16:00 local. Pre-existing consequence of the UTC sync — don't paper over it here.

kcal definition: `actKcal` (activity burn) is cached, `allKcal` (incl. resting) is not. A user comparing against the Even app's "calories" will see a smaller number. Label the surfaces "kcal", not "calories".

**How you'd know it worked.** Boot check first, no ring needed: `[R1-selftest] PASS activityDaily parse: recs=5 partial=2 steps=112 kcal=75`. Wrong numbers mean the layout or the shared reader regressed, and this fires before any BLE traffic. Then `ringconnect` and `ringquery activity daily`: the existing `[RING]   parsed: activityDaily …` line must be **unchanged in shape** (proves the annotator refactor is byte-identical) and the new `[RING] activity daily: page=… → steps=S kcal=K` line must appear. Run the query three times back to back — S and K must **converge**, not triple. Wear the ring, walk ~200 steps, re-query: S increases by roughly that, doesn't reset. Then the surfaces one at a time: OLED page renders inside 128 px with no clipping and wear still on the title line; lens Overview shows 7 lines with the status line **not** truncated (that's the `readout[208]` check — at 160 the last line gets cut); web `/r1-health` blocks populate and wrap at phone width. Finally build `ENABLE_G2_GLASSES=0` and `ENABLE_BLUETOOTH=0` to prove the stub struct moved in lockstep.

**Size.** A session, and realistically the longest of the five.

---

## Dependencies

**Fix 5 → everything.** It's detection-only and touches nothing the others touch, but its verdict decides whether adding daily traffic in Fix 4 and Fix 3 is safe. Land it and get a real answer before you tune Fix 4's settle window.

**Fix 1 before Fix 3, hard.** Fix 1 collapses `G2_RING_POLL_VITAL_COUNT` to a single definition above the `#if`. Any instruction that says "edit G2_Ring.h:145 and the stub at 220" is stale the moment Fix 1 lands — applied in that order it either fails to match or reintroduces the duplicate Fix 1 exists to remove.

**Fix 1 owns [System_SensorLogging.cpp:1414](components/hardwareone/System_SensorLogging.cpp:1414).** Both Fix 1 and Fix 3 prescribe the identical edit and each plan believes it's the owner. If Fix 3 lands it first, Fix 1 looks already-done and its comment/help-text/prose corrections get skipped. Give it to Fix 1 and delete it from Fix 3.

**Fix 2 before Fix 3, on the CSV schema.** Fix 2 establishes the interleaved `<vital>,<vital>_age_s` pattern; Fix 3 extends it. Reversed, you get two incompatible column layouts and two independent TEXT-buffer bumps that don't account for each other.

**Fix 4 before any sweep-table extension**, and it must grow `sDailyQueue` to 8 whether or not Fix 3 routes activity through it.

**The `healthstatus` strings are a three-way collision.** Fixes 1, 2 and 3 all rewrite the same `CommandEntry` description, usage block and `SUCCESS:` string with three different texts, and whoever lands last silently reverts the others. Write the final text **once**, after Fix 3, as the union, and regenerate [docs/COMMAND_REFERENCE.md](docs/COMMAND_REFERENCE.md) once at the end — not three times.

**Version.** RELEASING.md requires the version in four sites (`CMakeLists.txt` PROJECT_VER — currently `0.99.3`, README title, USERGUIDE title, QUICKSTART title) plus a CHANGELOG section, in one `vX.Y.Z: <plain summary>` commit. Fix 2 breaks the on-disk log format and Fix 3 adds user-visible data and bumps a JSON schema, so pre-1.0 SemVer sends this to **0.100.0**. Get the date from `date +%F`; don't guess it.

## What I would not do yet

**Don't extend `g2RingPollVital` to six steps.** This is the single change I'd hold hardest. It looks like a one-line `case 5:` and it is actually a traffic-model change applied to five callers at once, including one (`g2RingPollVitalForLogging`) that runs off every sensorlog sample with no page or interval gate, and one (the OLED page) that re-arms the whole burst automatically on every reconnect. Fix 4 is simultaneously spending a 20 s settle and 2 s pacing to keep that exact window quiet. Route activity/daily through the paced daily queue instead, and if Fix 5's detector confirms the anti-replay hypothesis, keep it off the shared ladder permanently.

**Don't write Phase 2 of the ring-retry fix until the detector reports a hit.** The clean end state is to store *intent* in the TX queue and re-serialize rather than re-queue raw bytes — but that changes `RingTxPending` and touches ~10 call sites. If the detector fires, the cheap first move is to make `ringWriteLocked` report *why* it failed ("never reached `writeValue`" vs "`writeValue` returned false"), and only push-front in the first case. ~15 lines, no call-site churn, removes the only in-session replay source. Every queued frame is a poll the next cadence re-issues, so dropping costs nothing.

**Don't add the host→ring heartbeat.** [docs/R1_RING_PROTOCOL.md](docs/R1_RING_PROTOCOL.md) §6.19 already records that we tried it, got no acknowledgment, and saw no change in link lifetime. A 30 s heartbeat is a fixed empty-payload frame whose only varying field is the serial — the most replay-prone frame we could add while the serial-reset question is open — and if its write fails it gets push-fronted and *becomes* the replay we're hunting. It also dilutes the detector's 30 s window with ~2 frames/min.

**Don't fix `healthlogmerge`'s CSV header stitching in this batch.** `cmd_healthlogmerge` byte-copies inputs, so stitching N CSV captures leaves N−1 header lines mid-file. Real defect, genuinely distinct, and it only becomes reachable *after* Fix 2. The help-text wording ("TEXT logs") is in scope; the header-skip is not.

**Don't add migration code for the old `docs/HealthCapture/` files.** Fourteen captures become unreadable by any parser written for the new format. Repo policy is a clean break — erase before flashing. Rename the directory or note the cutover.

## Where the prescription is still uncertain

Three things I'd flag rather than present as settled.

**Fix 3's on-ramp is the least-settled piece.** Both cross-checks independently rejected the poll-ladder approach and both landed on "route it through `queueDailyCmd`", which is the right instinct — but that queue was built for per-metric HR/HRV/SpO2/temp dailies driven by a page, and I have not traced what `g2RingQueryDaily(R1_CMD_ACTIVITY)`'s multi-frame reply does to the lens "fetching" indicator or to Fix 4's settle accounting when it's the *only* queued entry. Prototype the routing before committing to the design.

**Whether the `healthtrack` capture path should timestamp its filename is a product decision, not a bug.** Today it uses `CAPTURE_HEALTHLOG_DEFAULT` verbatim while the boot-resume path timestamps, so toggle cycles append to one file with one header. Timestamping it would be more correct and is a behaviour change that deserves its own call.

**Whether `healthtrack off` should un-coerce the format is also unresolved.** Leaving it means one Track toggle permanently moves a GPS/IMU capture from TEXT to CSV. Un-coercing means remembering the prior value, which is state nobody currently keeps.

**Fix 4's 8-second settle is a guess.** It was chosen to sit comfortably inside the ~19 s observed drop window, not derived from anything. Fix 5's `up=` / `rxAgo=` / `dupAgo=` telemetry is what turns it into a number.