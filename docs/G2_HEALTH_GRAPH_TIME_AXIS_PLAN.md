# Health graph: time-proportional X axis

## 1. Why it scrolls

Two mechanisms, and they compound. Neither is a repaint timer — I checked every arm site: the only paint path is [G2_Glasses.cpp:18704](components/hardwareone/G2_Glasses.cpp:18704), fully gated by the one-shot pending flag. Sitting on the page with no taps emits zero pushes. What you're seeing is that *every* repaint (and tapping a metric row is how you look at the graph) shows a materially shifted picture.

**Cause 1 — X is the array index, so every append re-spaces everything.** [G2_Health.cpp:726](components/hardwareone/G2_Health.cpp:726):

```cpp
int px_ = x + 1 + (int)((int64_t)(w - 3) * (int)i / (int)(n - 1));
```

`plotW = w - 3 = 247` ([:925](components/hardwareone/G2_Health.cpp:925): `kPlotW = 288 - 34 - 4 = 250`). Appending one sample displaces the interior by up to `247/n`: **49 px at n=5, 12 px at n=20, 2.6 px at n=96**. Once `count` saturates at 96 the eviction at [:132](components/hardwareone/G2_Health.cpp:132) translates the whole trace 2.6 px left per sample. Fixed by the time axis.

**Cause 2 — the 1 Hz resampler manufactures a sample every 5 s from a cache that never expires.** This is why cause 1 fires with zero ring input. [g2HealthTick](components/hardwareone/G2_Health.cpp:339) → [syncFromTelemetry:263](components/hardwareone/G2_Health.cpp:263) re-pushes cached values with `ringTs=0`; the only gate is [:126](components/hardwareone/G2_Health.cpp:126) `lastValue == value && (long)(ms - s->lastMs) < 5000`, and `lastMs` is refreshed on every *accepted* push. `g2RingGetTelemetry` ([G2_Ring.cpp:1679](components/hardwareone/G2_Ring.cpp:1679)) has no TTL on the `*Valid` flags. Result: one fabricated sample per 5 s per metric, forever. **96 slots refill in 8 minutes** and your real 24 h trail is evicted. Needs its own fix (part 2 below) — the time axis makes each phantom append move nothing, but 480 s of phantoms still walks the span up five rungs and destroys the data.

**Not a cause, contrary to what you might expect:** the two non-latching error branches at [G2_Glasses.cpp:18630](components/hardwareone/G2_Glasses.cpp:18630) and [:18648](components/hardwareone/G2_Glasses.cpp:18648) do spin at 5 Hz, but the tear-down-failure arm never reaches a CREATE and the `!bmp` arm has `bmp == null`, so neither can emit an image push. They're `BROADCAST_PRINTF` spam only.

**Also moving, and the time axis can't fix it:** [seriesAutoY:676](components/hardwareone/G2_Health.cpp:676) recomputes bounds per render with no hysteresis. One 95 bpm sample in a resting HR window remaps every point ~42 px vertically (31% of `kPlotH = 136`) and remaps it back on eviction. Optional fix in §5.

## 2. The change

**X domain: `HealthSample.ms`, as an unsigned modular offset from the oldest sample in the window, divided by the span snapped *up* to a coarse ladder rung (30 s … 7 d, roughly doubling).** Both terms are independent of the render instant, so a repaint with unchanged data reproduces the picture byte-for-byte and an append inside the current rung moves **0 px**. Density is real elapsed time: at the shipped ~903 s Health-Track cadence, 6 samples span 3000 s → 1 h rung → 41 px gaps; 96 samples span 57000 s → 1 d rung → 1–2 px gaps.

On "all-day denser than an hour" — be aware this holds through the *cadence→count* relationship (more elapsed time at fixed cadence = more samples = `plotW/(n-1)` shrinks), which today's index axis already delivers. 96 samples over 1 h and 96 over 24 h render at the same 2–3 px spacing under any data-fit axis. What the time axis actually buys you is (a) zero drift and (b) the trace visibly growing rightward into its window then rescaling when it outgrows it — which reads correctly without labels, and is why the *oldest* sample pins to the left edge rather than the newest to the right.

`.ringTs` is unusable as the domain: it's literally 0 for every sample from `syncFromTelemetry` ([:270-274](components/hardwareone/G2_Health.cpp:270)) and for both battery producers, and it carries two clock bases in one buffer (ring epoch from the point path at [G2_Ring.cpp:606](components/hardwareone/G2_Ring.cpp:606), host epoch from the bridge at [G2_Ring.cpp:782](components/hardwareone/G2_Ring.cpp:782)). It stays a dedupe key and day-label source.

No header change — nothing new is exported, so the `ENABLE_R1_HEALTH=0` stub block at [G2_Health.h:120-166](components/hardwareone/G2_Health.h:120) can't drift.

### 2a. Replace `seriesGetOrdered` ([:664-672](components/hardwareone/G2_Health.cpp:664))

The `int16_t vals[96]` copy discarded `.ms`. Index the static `.bss` ring buffer in place instead — nets **−96 B** of `g2_session_w` frame (−192 for the copy, +96 for the draw-order index). `kG2SessionStackBytes = 8192` and that is a *byte* count; this path is shared with the BMP/JPG workers. (Direction is arithmetic; I did not run a build, so absolute frame figures are unverified.)

```cpp
static size_t seriesOrderedStart(const HealthSeries* s, size_t* outCount) {
  const size_t n = s->count;
  if (outCount) *outCount = n;
  if (n == 0) return 0;
  return (s->head + HEALTH_HIST_CAP - n) % HEALTH_HIST_CAP;
}

static inline const HealthSample& seriesAt(const HealthSeries* s, size_t start, size_t i) {
  return s->buf[(start + i) % HEALTH_HIST_CAP];
}

// Unsigned modular delta, so a millis() rollover inside the window needs no
// special case (96 slots can never span the 49.7-day modulus). A future stamp
// — benign race with the notify task writing buf[] — reads as "now" rather
// than as a ~49.7-day age that would stretch the domain to a month.
static constexpr uint32_t kAgePlausibleMs = 30u * 24u * 3600u * 1000u;

static inline uint32_t sampleAgeMs(const HealthSeries* s, size_t start, size_t i,
                                   uint32_t nowMs) {
  const uint32_t age = (uint32_t)(nowMs - seriesAt(s, start, i).ms);
  return (age > kAgePlausibleMs) ? 0u : age;
}
```

`seriesAutoY` takes `(s, start, n)` instead of `(vals, n)`; body otherwise identical.

### 2b. The rung ladder

```cpp
// Span snaps UP to a rung, so appending doesn't re-space what's already drawn —
// the picture only rescales when the trail outgrows its rung. Roughly doubling
// on purpose: a finer ladder rescales more often, which is the motion this
// removes, and each step compresses the trace by at most 2x.
static const uint32_t kSpanRungsMs[] = {
     30u*1000u,    60u*1000u,   120u*1000u,   240u*1000u,   480u*1000u,
    900u*1000u,  1800u*1000u,  3600u*1000u,  7200u*1000u, 14400u*1000u,
  28800u*1000u, 43200u*1000u, 86400u*1000u,172800u*1000u,345600u*1000u,
 604800u*1000u,
};

static uint32_t seriesSpanWindowMs(uint32_t spanMs) {
  for (size_t i = 0; i < sizeof(kSpanRungsMs)/sizeof(kSpanRungsMs[0]); i++) {
    if (spanMs <= kSpanRungsMs[i]) return kSpanRungsMs[i];
  }
  return spanMs;  // past the ladder — exact fit rather than clipping
}
```

12 rungs exist between 30 s and 1 d, but a given cadence only crosses the subset up to its saturated span — 3–4 crossings over a 16 h fill at a 10-min cadence.

### 2c. `drawSparkline` ([:702-752](components/hardwareone/G2_Health.cpp:702))

Signature unchanged, so the call at [:930](components/hardwareone/G2_Health.cpp:930) and `metricParams`' `const HealthSeries**` are untouched.

```cpp
static_assert(HEALTH_HIST_CAP <= 255, "draw order index is uint8_t");

static void drawSparkline(const HealthSeries* s, int x, int y, int w, int h,
                          int16_t softMin, int16_t softMax, int16_t minSpan,
                          int shade, int16_t* usedMin, int16_t* usedMax) {
  if (!s || s->count < 2 || w < 4 || h < 4) return;

  size_t n = 0;
  const size_t start = seriesOrderedStart(s, &n);
  if (n < 2) return;

  int16_t yMin = softMin, yMax = softMax;
  seriesAutoY(s, start, n, softMin, softMax, minSpan, &yMin, &yMax);
  if (usedMin) *usedMin = yMin;
  if (usedMax) *usedMax = yMax;
  if (yMax <= yMin) yMax = (int16_t)(yMin + 1);

  drawHLine(x, x + w - 1, y, 3);
  drawHLine(x, x + w - 1, y + h - 1, 3);
  drawVLine(x, y, y + h - 1, 3);
  drawVLine(x + w - 1, y, y + h - 1, 3);

  const uint32_t nowMs = millis();

  // Draw order is TIME order, not insertion order: daily backfill appends
  // records OLDER than the live samples already in the buffer, so an
  // index-ordered polyline jumped from the newest live point back to the start
  // of history. 96 B of index still beats the 192 B value copy this replaced.
  uint8_t order[HEALTH_HIST_CAP];
  for (size_t i = 0; i < n; i++) order[i] = (uint8_t)i;
  for (size_t i = 1; i < n; i++) {          // insertion sort, oldest first
    const uint8_t v = order[i];
    const uint32_t va = sampleAgeMs(s, start, v, nowMs);
    size_t j = i;
    while (j > 0 && sampleAgeMs(s, start, order[j - 1], nowMs) < va) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = v;
  }

  const uint32_t ageMax = sampleAgeMs(s, start, order[0], nowMs);
  const uint32_t ageMin = sampleAgeMs(s, start, order[n - 1], nowMs);
  const uint32_t spanMs = ageMax - ageMin;
  // A zero span (degenerate daily payload, or one tick's worth of samples) has
  // no domain at all — space those evenly so the trail is still legible.
  const bool uniform    = (spanMs == 0);
  const uint32_t windowMs = uniform ? 1u : seriesSpanWindowMs(spanMs);  // 1 = unused
  const int plotW = w - 3;
  constexpr int kDotMinGapPx = 4;

  int prevX = -1, prevY = -1;
  for (size_t k = 0; k < n; k++) {
    const size_t i = order[k];
    int px_;
    if (uniform) {
      px_ = x + 1 + (int)((int64_t)plotW * (int)k / (int)(n - 1));
    } else {
      const uint32_t off = ageMax - sampleAgeMs(s, start, i, nowMs);
      px_ = x + 1 + (int)((uint64_t)(uint32_t)plotW * off / windowMs);
    }
    int clamped = seriesAt(s, start, i).value;
    if (clamped < yMin) clamped = yMin;
    if (clamped > yMax) clamped = yMax;
    const int py = y + h - 2 - (int)((int64_t)(h - 3) * (clamped - yMin) / (yMax - yMin));
    if (prevX >= 0) {
      if (px_ == prevX && py == prevY) continue;   // same pixel — nothing to add
      const int dx = px_ - prevX, dy = py - prevY;
      const int adx = dx < 0 ? -dx : dx;
      const int ady = dy < 0 ? -dy : dy;
      int steps = adx > ady ? adx : ady;
      if (steps < 1) steps = 1;
      // From step 1: step 0 is the previous sample's own pixel, and repainting
      // it at stroke shade was erasing that sample's bright dot.
      for (int sstep = 1; sstep <= steps; sstep++) {
        const int xx = prevX + dx * sstep / steps;
        const int yy = prevY + dy * sstep / steps;
        px(xx, yy, shade);
        px(xx, yy + 1, shade);
        // Thicken only runs that travel sideways; on a dense trail the segments
        // are near-vertical min/max bars and widening those bleeds into the
        // next sample's column.
        if (shade > 2 && adx > 1) px(xx + 1, yy, shade - 2);
      }
    }
    if (prevX < 0 || px_ - prevX >= kDotMinGapPx) {
      px(px_, py, 15);
      px(px_ + 1, py, 12);
      px(px_, py + 1, 12);
    }
    prevX = px_;
    prevY = py;
  }
}
```

Note the three draw-code changes are separable from the axis work — `sstep` starting at 1 is a pre-existing dot-erasure fix, and it alters every sparse trail. Split it if you want a clean regression attribution.

### 2d. Backfill timestamps (prerequisite, [:203-211](components/hardwareone/G2_Health.cpp:203) and [:225-233](components/hardwareone/G2_Health.cpp:225))

The current `ms = nowMs - (count-1-i)*60000u` asserts a flat 60 s ladder for a payload that nominally spans a day, so a 24 h backfill occupies 52% of the plot instead of 99%. Insert before [:191](components/hardwareone/G2_Health.cpp:191):

```cpp
static constexpr uint32_t kBackfillNominalStepMs = 60000u;      // window unusable
static constexpr uint32_t kBackfillMaxSpanSec    = 48u * 3600u; // reject garbage windows

static uint32_t backfillSampleMs(size_t i, size_t count, uint32_t nowMs,
                                 uint32_t startTs, uint32_t endTs) {
  if (count < 2) return nowMs;
  uint32_t spanMs = (uint32_t)((count - 1) * kBackfillNominalStepMs);
  if (endTs > startTs && (endTs - startTs) <= kBackfillMaxSpanSec) {
    spanMs = (endTs - startTs) * 1000u;
  }
  // No clamp against nowMs: .ms has exactly two consumers, seriesPush's
  // (long)(ms - lastMs) signed delta and sampleAgeMs's unsigned delta. Both are
  // modular, so an early-uptime wrap recovers the exact age. kBackfillMaxSpanSec
  // keeps the derived age well under kAgePlausibleMs.
  return nowMs - (uint32_t)((uint64_t)spanMs * (count - 1 - i) / (count - 1));
}
```

In both loops, replace the `rts` / `ms` lines with:

```cpp
    const uint32_t rts = span ? (startTs + (uint32_t)((uint64_t)span * i / (count - 1))) : 0;
    const uint32_t ms  = backfillSampleMs(i, count, nowMs, startTs, endTs);
```

and in `g2HealthApplyDailyBackfill` switch `seriesPush` → **`seriesPushRaw`**. Reason: a degenerate window (`endTs <= startTs` or `count == 1`) leaves `span == 0`, every record gets an identical `ringTs`, and [:125](components/hardwareone/G2_Health.cpp:125) `if (ringTs != 0 && s->lastRingTs == ringTs) return;` drops records 1..N-1 — **64 records in, 1 stored**, and the tile renders "no samples / Poll Now" for a metric the ring just answered in full. That's a live data-loss bug today. Passing `rts = 0` bypasses the identity gate but the value+time gate then collapses runs of equal adjacent values (daily HR/SpO2 payloads have plenty), so `seriesPushRaw` is the complete fix. Safe because the `s->count >= kThinHistory` guard at [:197](components/hardwareone/G2_Health.cpp:197) already prevents a second backfill.

Still unfixed and out of scope: when `0 < span < count-1` seconds, the interpolated `rts` values collide and the identity gate drops those records.

### 2e. Kill the phantom resampler (required, or the trail still grows with no ring data)

**Do not derive `.ms` from `t.*AgeSec`.** [`ringSampleAgeSec`](components/hardwareone/G2_Ring.cpp:333) *prefers* the ring epoch and only falls back to `(millis() - rxMs)/1000`, so a ring clock behind the host parks a fresh sample hours in the past (stretching the domain) and a ring clock ahead hits `if (a < 0) a = 0;` — age pins at 0, the derived stamp is `nowMs` every tick, and the 5 s phantom appends come straight back. Ring-clock custody steps the ring both directions by design.

Use the receive stamp instead. Add to `G2RingTelemetry` in **both** branches of G2_Ring.h ([:153](components/hardwareone/G2_Ring.h:153) and [:249](components/hardwareone/G2_Ring.h:249) — `ENABLE_R1_HEALTH` is force-zeroed on BT-off/G2-off boards, so a green FeatherS3 build won't compile the stub):

```cpp
  // millis() at local receive, 0 = unknown. Monotonic and immune to
  // ring-clock custody, unlike *AgeSec which prefers the ring epoch.
  uint32_t hrRxMs, hrvRxMs, spo2RxMs, tempRxMs, batteryRxMs;
```

Copy them straight through in [`g2RingGetTelemetry`](components/hardwareone/G2_Ring.cpp:1679) from the `gR1Cache.*RxMs` fields already there ([G2_Ring.cpp:289-293](components/hardwareone/G2_Ring.cpp:289)), then rewrite [:263-275](components/hardwareone/G2_Health.cpp:263):

```cpp
// Live-cache resample. The cache has no TTL, so a re-read of an unchanged
// reading must not append: on a time axis a fabricated sample crowds the trail,
// and with 96 slots it evicts real history — eight minutes of page dwell used
// to replace a whole day with copies of one cached value. Stamping each sample
// with its own RECEIVE time makes seriesPush's value+time gate do the right
// thing: an unchanged cache keeps producing the same stamp and is dropped
// forever, while a genuinely new reading lands.
static void syncPush(HealthSeries* s, int16_t value, uint32_t rxMs) {
  if (rxMs == 0) return;   // no receive time known — don't invent one
  seriesPush(s, value, 0, rxMs);
}

static void syncFromTelemetry(void) {
  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  if (!t.connected) return;
  if (t.hrValid)      syncPush(&sHr,   (int16_t)t.hr,      t.hrRxMs);
  if (t.hrvValid)     syncPush(&sHrv,  t.hrv,              t.hrvRxMs);
  if (t.spo2Valid)    syncPush(&sSpo2, (int16_t)t.spo2,    t.spo2RxMs);
  if (t.tempValid)    syncPush(&sTemp, t.tempTenths,       t.tempRxMs);
  if (t.batteryValid) syncPush(&sBat,  (int16_t)t.battery, t.batteryRxMs);
}
```

Same millis domain as the notify path's `g2HealthNoteSample`, so the X axis stays coherent. Expect one visible behaviour change: a metric holding a single cached reading now stays at `count 1` and renders "no samples / Poll Now" instead of a flat line built from 96 copies. Per the overnight results the ring never answers temp point queries, so **Temp is the metric that will look different** — it'll be fed only by daily backfill.

## 3. Edge cases

| Case | Behaviour |
|---|---|
| `spanMs == 0` (all stamps equal — degenerate payload, or one tick's samples) | `uniform` branch: even index spacing across the full width. `windowMs` set to 1, never a live divisor, so no divide-by-zero. |
| `n == 2` | Normal rung mapping, **not** a special case. A 5 s pair draws as a short stub at the left of a 30 s rung. Deliberate: forcing full width at `n<3` makes the 2→3 transition snap the middle point 165 px — larger than any rung crossing, and it fires on first view of every metric. |
| `n < 2` / `count < 2` | Early return, unchanged; `composeMetricGraph` renders "no samples". |
| Degenerate backfill window (`endTs <= startTs`, `count == 1`) | `rts = 0` plus `seriesPushRaw` — all records land. Previously 1 of 64 survived. |
| Backfill `.ms` underflow at early uptime | Harmless. `nowMs - 3,780,000` at 60 s uptime gives `4,291,247,296`, and `sampleAgeMs` recovers exactly `3,780,000`. Modular subtraction, order preserved. No clamp — a clamp would make scale a function of *uptime* (a 24 h payload would collapse to 3% of the plot at 1 s uptime). |
| `millis()` rollover inside the window | No special case. Every quantity is `(uint32_t)(nowMs - ms)` differenced against another such value; a series straddling `0xFFFFF000 → 0x00003A98` renders strictly ascending with the oldest exactly on the left edge. |
| Future-dated stamp (notify task writes `buf[]` while the renderer reads) | `sampleAgeMs` returns 0 above `kAgePlausibleMs` (30 d), so it reads as "now" instead of stretching the domain to 49.7 days. |
| One genuinely much-older sample | Rendered honestly — the recent cluster squashes to the right. Clamping a minimum spacing would break the density property. Mostly an artefact of the phantom resampler anyway. |
| `count == 96`, eviction per sample | `t_oldest` advances one cadence interval, translating the trace ~2.6 px per *genuinely new* sample at the 903 s default. Unavoidable for a fixed-capacity newest-wins buffer, and it's real data arriving 15 minutes apart. |
| Rung crossing | Up to 124 px (`plotW/2`) whole-trace compression, 3–4 times over a 16 h fill. This *is* "shrink and grow in scale". `kSpanRungsMs` is the single knob — halving the spacing halves the jump at ~1.7× the rescale count. |
| Torn read of `buf[i]` | Pre-existing, unchanged, no lock added (matches the documented "worst case is one stale tick"). Strictly more robust than today because the future-stamp guard neutralises the one visible torn-read pattern. |

## 4. The bonus: backfill-draws-in-the-wrong-place

**Yes, this fixes it, and the timestamps are comparable — I verified rather than assumed.** Both [:202](components/hardwareone/G2_Health.cpp:202) and [:224](components/hardwareone/G2_Health.cpp:224) do `const uint32_t nowMs = millis();` and the synthesised `ms` is derived from that. Ring epochs only ever reach `rts` ([:205-206](components/hardwareone/G2_Health.cpp:205), [:231-232](components/hardwareone/G2_Health.cpp:231)), never `ms`. So backfilled records genuinely sort before live ones on a `.ms` axis, and **no ingest-side prepend is needed** — the age-sorted draw order makes the renderer correct for any insertion order, which is a stronger guarantee than a prepend.

This matters because backfill-after-live is the *normal* path: [:197](components/hardwareone/G2_Health.cpp:197) only backfills while `count < kThinHistory` (8), so 1–7 live samples sit ahead of up to 64 older records, and the index axis was drawing that older history to the *right* of the newer points.

The Trends series are safe on the same domain but carry no real per-record time — `.ringTs` is a uniform interpolation and `.ms` is derived from the payload window, so a per-sample time axis there degenerates to even spacing. The only real information a Trends day has is its *duration*, and `backfillSampleMs` now honours it: a 24 h window fills a 1 d rung to 99% instead of a 2 h rung to 52%. Note Trends caps at 64 (`R1DailyResult::values[64]`, clamped in `r1ParseHealthDaily`), not 96, so a Trends day renders visibly sparser than a live trail.

## 5. Optional polish

Neither is required for the axis change; both are independent and can land later.

**Y-axis grid snap.** Replace `seriesAutoY`'s `pad = (hi - lo) / 8` with snapping `lo` down and `hi` up to `step = max(minSpan/2, 1)` — HR 6 bpm, HRV 5 ms, SpO2 2%, Temp 0.5 °C, Battery 2%. A wobble inside one grid cell then moves the axis, every plotted point, and all three gutter labels by exactly zero. Two corrections to note: your headline case (HR 72..78 vs 73..77) is *already* stable today because `minSpan = 12` dominates the `span < minSpan` branch — the churn case is `span >= minSpan`, e.g. 60..80 vs 60..81. And the snap widens HR from today's 68..82 (14 units) to 66..84 (18), a ~29% vertical resolution loss on 136 px. Add `int16_t axisLo, axisHi` to `HealthSeries` (+32 B internal DRAM total — that's per-*series*; a per-*sample* field would be 96×8×4 = 3072 B) with a one-step narrowing deadband if edge-flap survives. If you do: those fields are written from the render path on `g2_session_w` and zeroed by `seriesClear` on the notify task, so it's one-wrong-frame racy, not single-task. And snap `softMin`/`softMax` to the grid too, or the clamp knocks the bound off-grid at axis extremes (HR `softMax = 200`, step 6).

**Push-skip fingerprint.** Fold FNV-1a over the packed bytes inside [`healthPackBmp:1236`](components/hardwareone/G2_Health.cpp:1236) (`rowStride = 144 = w/2`, zero pad bytes, so hashing pixels == hashing payload) and have `g2RenderHealthBmp` return 0 when it matches what's on the lens. Skips the ~20 KB burst on a re-tap of the already-selected metric and on a poll that returned unchanged values — same idiom as the existing `strcmp(readout, lastReadout)`, placed where the buffer lives. Two rules it must obey: commit the hash only on a *confirmed* push (a failed burst must stay eligible for retry), and invalidate on container tear-down (otherwise Overview→HR→Overview→HR with no new data skips onto a blank container). Marginal cost is ~0.2–0.4 ms on Xtensa, not the 0.09 ms a 1-cycle/byte estimate suggests. Needs stubs in both `G2_Health.h` branches.

## 6. What I'd leave alone

- **Fixed-width time window** ("always the last hour"). Fails all three goals: the right edge pinned to `millis()` marches points left at `plotW/window` px per *second* with no new data — a literal strip chart, a purer form of your complaint. And at equal cadence an all-day and an hour-long log both show only their last hour, so "all-day denser" becomes unreachable, not just unmet.
- **Exact data-fit with the newest pinned right.** At uniform cadence this is mathematically the index axis: appending displaces the interior by `plotW/n` = 2.6 px at n=96. Kept only as the past-the-ladder fallback.
- **Anchoring at a fixed session start.** The anchor sample is evicted once the buffer saturates, so the left portion of the plot goes permanently and increasingly blank.
- **A parallel `uint32_t ts[96]`.** +384 B of `g2_session_w` frame. Indexing `.bss` in place is smaller and simpler.
- **Sticky per-series rung with hysteresis.** Flapping needs the span to oscillate across a boundary by more than the inter-rung gap; with a doubling ladder that's seconds against hours. Keeping `drawSparkline` a pure function of the buffer is worth more.
- **A cooldown between graph pushes.** Makes deliberate taps feel broken while doing nothing about a *changed* picture. Possibly still justified as separate crash-hardening on the fragment path (the TLSF crash-after-lens-spam note), but it's not an anti-churn fix.
- **Latching `shapeOverview`/`menuGen` in the `!bmp` branch** at [G2_Glasses.cpp:18648](components/hardwareone/G2_Glasses.cpp:18648). Tempting, but worse than the bug: the container was already torn down at [:18629](components/hardwareone/G2_Glasses.cpp:18629) and nothing was created, so the loop falls into the text branch, writes to a widget that doesn't exist, and never sets the dead flag — a permanently blank lens plus a flag that lies about container state, with no recovery since scratch acquire runs once per session. If you fix it, restore Overview or `break` out of the page.
- **`queueDailyCmd`'s tail-only scan** at [:154-160](components/hardwareone/G2_Health.cpp:154) — a re-tap after drain re-queues the same daily and arms a second 20 KB push. Pre-existing, separate.
- **`g2HealthHistoryCount`** at [:258](components/hardwareone/G2_Health.cpp:258) has zero consumers tree-wide. Deletion candidate for a later sweep.

## 7. Size and hardware verification

Five edits in [G2_Health.cpp](components/hardwareone/G2_Health.cpp) (~130 lines net) plus five fields and their copy-through in [G2_Ring.h](components/hardwareone/G2_Ring.h) / [G2_Ring.cpp:1679](components/hardwareone/G2_Ring.cpp:1679) for §2e. No `G2_Health.h` change. Net −96 B of `g2_session_w` frame, +20 B `G2RingTelemetry`, no PSRAM, no new allocation, nothing on the BTC-task stack. The sort is ~4600 modulo ops (~50 µs) against a ~20 KB BLE burst. All eight views that share `composeMetricGraph` (MAIN HR/HRV/SpO2/Temp/Battery, Trends HR/HRV/SpO2) get it from the one change; OLED_Mode_R1_Health.cpp, WebPage_R1_Health.cpp, the Health Track CSV writer and the unrelated `sparkline` matches in WebPage_Games.h are all untouched.

On hardware:

1. Open a metric with a rich trail, note the trace, wait 2 minutes without tapping, re-tap the same row — **pixel-identical**. That's the whole fix in one test.
2. Sit on one metric for 10 minutes, then check the `n=` counter in the text pane ([:1176](components/hardwareone/G2_Health.cpp:1176)) — with §2e it must **not** climb to 96.
3. Compare a freshly-booted metric (few samples, sparse dots, trace filling part of the width) against one after several hours (dense band, near-full width) — and watch for the rescale as the span crosses a rung.
4. Tap a Trends metric: the day graph should now span ~99% of the plot, not ~52%.
5. Force a thin series and let a daily land (tap HR on a fresh series): expect ~64 points, not "no samples" — that's the `seriesPushRaw` fix. Do this within the first hour of uptime so the underflow path is exercised.
6. Build a BT-off or G2-off board once, since a green FeatherS3 build doesn't compile the `G2_Ring.h` stub branch you just edited.

## 8. The labeled axis (implemented after §2 shipped)

§2 argued the trace "reads correctly without labels." That held for drift but not for scale: a 15-minute trail and a 24-hour trail render as the same dense band, and a rung crossing silently halves the scale with nothing on the tile saying so. The axis fixes the disambiguation problem the trace alone can't.

**Geometry.** The plot ceded 14 px: `kPlotH` 136 → 122 (plot rows 4..125), tick stubs at rows 126..128 under the left/mid/right columns of the bottom border (6/5/6 shades, mirroring the Y ticks), 2× labels at rows 131..140. Left label left-aligned at the plot edge, mid centered, right right-aligned ending on the plot's last column. Widest real label is 5 glyphs = 38 px; three of them use 114 of 250 px, so no collision handling is needed. 1× text was already rejected as unreadable on the lens (see the empty-state comment), so 2× is not negotiable and the 14 px cost (~10% vertical resolution) is the price.

**The axis labels the WINDOW, not the trace tip** — left edge = oldest sample, right edge = oldest + rung. All three positions are constants; the text changes only when the data genuinely reframes (eviction, rung crossing, backfill). That keeps §2's contract intact: append inside the rung → zero pixels move, labels included; rung crossing → trace and labels rescale together, which is exactly the moment the user needs telling.

**Three modes** (`drawTimeAxis`, each a pure function of buffer + window, never of the render instant):

- **Trends** — hours into the ring's UTC day from `TrendMeta.startTs` (`0:00 / 12:00 / 24:00` on a full day; `24:00` deliberate — wrapping the window's end to `0:00` would relabel it as its start). Trend `.ms` is fetch-anchored (`backfillSampleMs` pins the newest record at the fetch instant), so wall time derived from `.ms` would print the *fetch* time; the meta is the only honest source. Labels are skipped when the meta window fails backfill's own honoured-window gate (`endTs > startTs && span ≤ kBackfillMaxSpanSec`) — in that case the trace was laid on the nominal 60 s ladder and day labels would misplace it — and when `startTs` isn't a plausible epoch (never-synced ring).
- **Live, synced host, window ≥ 5 min** — local wall clock, `H:MM` under 48 h, `M/D` at the multi-day rungs. Derived as `Clock::epochMillis() − oldestAgeMs`: both clocks advance in lockstep, so the value is repaint-stable and steps only when the clock itself steps (NTP, ring-clock custody) — which *should* reframe the axis.
- **Elapsed-from-start** (`0 / 2h / 4h`) — dark host, or a window below 5 min where `H:MM` marks wouldn't be distinct (30–240 s rungs). Anchored to the window start, not "now", so even a dark-host repaint stays pixel-identical. Formatting is `fmtElapsedLabel`, not `fmtAge` — fmtAge truncates (7.5 m → "7m"), fine for a sample age, wrong for an axis mark; rung values and their halves are exact in one decimal everywhere the ladder can put a tick.

**Plumbing.** `drawSparkline` exports its already-computed domain via two out-params (`outOldestAgeMs`, `outWindowMs`; window 0 = uniform/degenerate → axis suppressed), mirroring how `usedMin`/`usedMax` feed the Y labels. `composeMetricGraph` gained a trailing `const TrendMeta*` default-nullptr parameter; the three Trends call sites pass their metas. Two glyphs added (`h`, `d`) for elapsed labels. Nothing new is exported — the `ENABLE_R1_HEALTH=0` stub branch is untouched.

**Backfill taint (found in adversarial review, fixed).** Live series seeded by `g2HealthApplyDailyBackfill` (Temp always; HR/HRV/SpO2 when thin) inherit `.ms`'s fetch anchoring, so wall labels would have printed times shifted late by (fetch instant − ring's `endTs`) — hours wrong with a stale payload (ring off-wrist, host freshly booted). Rather than re-anchoring backfill `.ms` at ingest (leaves a hole when the host is dark at fetch and syncs later; makes `.ms` semantics depend on sync state), the series carries `hasBackfill` + `backfillAnchorMs`: while the time-oldest sample doesn't post-date the fetch instant, the axis stays on elapsed labels; once every pre-backfill slot evicts, the signed diff goes positive and wall labels return on their own. Repaint-stable — the mode flips only on backfill/eviction, which are permitted reframes. The `backfillSampleMs` "two consumers" comment is updated to name the axis as the third.

**Three more review-driven guards:** `fmtDayOffsetLabel` wraps past-24 h ticks to next-day time-of-day (`26:00` → `2:00`; exact `24:00` kept) for the defensive case of a non-midnight-aligned ring window; the 24 h rung — the one H:MM window whose endpoints print identical text — appends the date to the right label (`7:41 … 7:41 7/30`); and wall labels derive from a latched epoch↔millis offset (`wallOffsetMs`, 1 s step hysteresis) instead of two live clock reads, because independent ms-truncation of `epochMillis()`/`millis()` could otherwise flip a printed minute between two repaints of an identical buffer. Trends additionally skips labels for a sub-2-minute "day" window (all three marks would truncate to the same H:MM).

**Hardware checks for the axis specifically:**

1. Re-tap a rich metric with no new data, synced host — pixel-identical including labels.
2. Overnight Health-Track trail — left label reads yesterday evening, right = left + rung; watch one rung crossing: trace and labels rescale in the same push.
3. Trends on a complete ring day — `0:00 / 12:00 / 24:00`; today's partial day — same frame, trace filling proportionally.
4. Dark boot — elapsed labels (`0 / …`); when NTP or ring-clock custody lands mid-session, next push reframes to wall clock once.
5. Temp (backfill-fed) — axis present in **elapsed** form (`0 / … `), not wall clock: that's the backfill taint doing its job. HR after an early-session backfill — elapsed at first, flipping to wall labels on its own once the synthetic slots evict (~96 pushes later).
6. A metric with exactly 2 samples seconds apart — 30 s rung, elapsed labels `0 / 15s / 30s`, no `H:MM` degenerate pair.
7. A full-day live trail on the 24 h rung — right label carries the date cue (`7:41 … 7:41 7/30`).