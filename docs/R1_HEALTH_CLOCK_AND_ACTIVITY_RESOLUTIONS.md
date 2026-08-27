## #2 — Clock basis in the durable health log

### The resolution

Emit six absolute per-metric ring stamps in epoch-**milliseconds**, interleaved as `value,stamp` pairs, using the same dual-range idiom as the row stamp at [System_SensorLogging.cpp:467](components/hardwareone/System_SensorLogging.cpp:467), plus one trailing `r1_ts_src` bitmask saying whether each stamp is the instant the ring measured the sample or the instant we received it. No ages in CSV. TEXT stays deliberately asymmetric — fused `@N` / `@~N` / `@?` suffixes — and both `512` literals in the TEXT builder collapse to one `kTextRowCap = 1024`. The format-coercion fix ships in the same change, but **not** as a refusal on the set path: pin CSV at session *start*.

Two things in my earlier reasoning were wrong and are corrected below: the tier-2/tier-3 stamps must be **captured at receive**, not recomputed at write time, and the Health-Track CSV pin must not live in the format setter.

### Why timestamps and not ages

An age can't identify a sample. The complaint was that a fresh notify and a ten-minute-old cache re-read are byte-identical rows; ages give them `0` and `600` and still leave them indistinguishable as the *same* physical reading, which is exactly what makes a grapher plot one HR value forty times. A stamp plus the row stamp recovers the age; the age never recovers the instant. And `ringSampleAgeSec` at [G2_Ring.cpp:333](components/hardwareone/G2_Ring.cpp:333) bakes its basis choice into the number with nothing marking it — while `ringAdoptClockIfDark` at [G2_Ring.cpp:394](components/hardwareone/G2_Ring.cpp:394) can flip that basis mid-session.

One correction to how I argued the unit. I claimed epoch-seconds was *impossible*; it isn't, and my number was off. The collision is only between epoch-seconds and the tier-3 boot-ms fallback **sharing one column** — and it opens at **18.25 days** uptime (boot-ms crossing the 2020 `isValidEpoch` floor of 1.5778e9), not 20.5, running to the 49.7-day wrap. A single-basis seconds column with empty cells would have no collision at all. Milliseconds still win, but on the honest reason: they keep tier 3 in the same column self-disambiguating, and epoch-ms is already the convention the row stamp and per-day file continuity are built on.

### The code

**CSV column order — LOG_R1 segment, 7 → 14 columns.** Header at [System_SensorLogging.cpp:159](components/hardwareone/System_SensorLogging.cpp:159), row builder at [System_SensorLogging.cpp:535](components/hardwareone/System_SensorLogging.cpp:535). I counted these twice against each other:

```
r1_connected,
r1_hr,      r1_hr_ts_ms,
r1_hrv,     r1_hrv_ts_ms,
r1_spo2,    r1_spo2_ts_ms,
r1_temp,    r1_temp_ts_ms,
r1_battery, r1_battery_ts_ms,
r1_wear,    r1_wear_ts_ms,
r1_ts_src
```

```c
written = snprintf(pos, remaining,
                   ",%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%u",
                   s.r1Connected ? 1 : 0,
                   hr, hrTs,  hrv, hrvTs,  spo2, spo2Ts,
                   temp, tempTs,  bat, batTs,  wear, wearTs,
                   (unsigned)s.r1TsSrcMask);
```

14 names / 14 conversions / 14 args. Interleaved rather than a grouped block for the reason your own comment about `r1_temp` sitting between spo2 and battery already documents: a future metric is one pair in each place and the pairing can't be broken by miscounting.

`r1_ts_src` bits: `1 hr, 2 hrv, 4 spo2, 8 temp, 16 battery, 32 wear`. Set = ring-measured. Note bits 64/128 are the only headroom left — see the interaction section.

**The stamps must be frozen at receive.** This is the real fix to my first pass. `R1TelemetryCache` at [G2_Ring.cpp:288](components/hardwareone/G2_Ring.cpp:288) gains a per-metric companion:

```c
// The receive instant, captured ONCE when the frame lands, plus whether the
// metric's *Ts field is the ring's own measurement instant. Both frozen at
// receive on purpose: deriving them at write time (epochMillis() - (millis()-rxMs))
// re-bases every stamp on an NTP step or a ring-clock adoption, with no new
// sample — which destroys the sample identity these columns exist to provide,
// and makes the R1 dedup key jitter every tick.
struct R1MetricStamp {
  int64_t rxEpochMs;   // wall-clock receipt, 0 if the host was dark
  bool    tsFromRing;  // *Ts is the ring's clock, not ours
};
```

and the resolver becomes a pure read:

```c
static int64_t ringResolveStampMs(uint32_t ringTs, const R1MetricStamp& st,
                                  uint32_t rxMs, bool* fromRing) {
  if (fromRing) *fromRing = false;
  if (st.tsFromRing && ringPlausibleEpoch(ringTs)) {
    if (fromRing) *fromRing = true;
    return (int64_t)ringTs * 1000LL;          // tier 1: ring measured it
  }
  if (st.rxEpochMs != 0) return (st.rxEpochMs / 1000LL) * 1000LL;  // tier 2
  if (rxMs != 0)         return (int64_t)rxMs;                     // tier 3: dark host
  return 0;
}

static inline void ringNoteRx(R1MetricStamp& st, bool fromRing) {
  st.rxEpochMs  = Clock::isSynced() ? Clock::epochMillis() : 0;
  st.tsFromRing = fromRing;
}
```

A sample received dark keeps `rxEpochMs = 0` forever, so its stamp stays boot-ms even after the clock comes up — the row stamp is then 13-digit, the magnitudes visibly disagree, and CSV/TEXT correctly decline to subtract. That is the honest outcome and it is *stable*, which the write-time derivation was not.

**`tsFromRing` is load-bearing on the forwarded path.** [G2_Ring.cpp:811](components/hardwareone/G2_Ring.cpp:811) does `gR1Cache.hrTs = (sv > 0) ? (uint32_t)sv : now;` — `now` is `time(nullptr)`, our clock. Without the flag, tier 1 would emit a *host* stamp with the ring bit **set**: a provenance lie. So:

```c
      case 3:  // hr — value only; the ts (field 4) may or may not follow
        if (sv > 0 && sv < 250) {
          gR1Cache.hr = (uint8_t)sv; gR1Cache.hrRxMs = rx; gR1Cache.hrValid = true;
          ringNoteRx(gR1Cache.hrStamp, false);   // no ring ts yet
          g2HealthNoteSample(HEALTH_METRIC_HR, (int16_t)sv, now);
        }
        break;
      case 4:  // hrTs — a real forwarded ring ts promotes provenance; `now` does not
        if (gR1Cache.hrValid) {
          gR1Cache.hrTs = (sv > 0) ? (uint32_t)sv : now;
          gR1Cache.hrStamp.tsFromRing = (sv > 0);
        }
        break;
```

Same shape for 6/8/10. The point branch at [G2_Ring.cpp:616](components/hardwareone/G2_Ring.cpp:616) calls `ringNoteRx(..., ts != 0)`; deviceStatus at [G2_Ring.cpp:712](components/hardwareone/G2_Ring.cpp:712) and `ringNoteWear` at [G2_Ring.cpp:314](components/hardwareone/G2_Ring.cpp:314) call it with `false` — battery and wear are structurally tier 2/3 and their mask bits never set.

**Dedup: tier-1 stamps only.** My first pass put all six stamps in the key. `batteryRxMs` and `wearRxMs` are rewritten on every `deviceStatus` reply *and* every unsolicited heartbeatPack, so that would have disabled the R1 dedup at [System_SensorLogging.cpp:786](components/hardwareone/System_SensorLogging.cpp:786) outright — one row per poll cycle, not "somewhat more rows". Gate each term on its provenance bit and omit battery/wear entirely:

```c
    const bool r1Changed = (flags != lastR1Flags) ||
        /* …existing value terms… */ ||
        (((snap.r1TsSrcMask & G2RING_TSSRC_HR)   != 0) && snap.r1HrTsMs   != lastR1HrTs) ||
        (((snap.r1TsSrcMask & G2RING_TSSRC_HRV)  != 0) && snap.r1HrvTsMs  != lastR1HrvTs) ||
        (((snap.r1TsSrcMask & G2RING_TSSRC_SPO2) != 0) && snap.r1Spo2TsMs != lastR1Spo2Ts) ||
        (((snap.r1TsSrcMask & G2RING_TSSRC_TEMP) != 0) && snap.r1TempTsMs != lastR1TempTs);
```

A re-measured 62 bpm fifteen minutes later still writes a row; delivery timing no longer drives the key.

**Buffers.** Both literals — `ps_alloc(512, …)` at [System_SensorLogging.cpp:330](components/hardwareone/System_SensorLogging.cpp:330) and `int remaining = 512;` at [System_SensorLogging.cpp:334](components/hardwareone/System_SensorLogging.cpp:334) — become one `static constexpr size_t kTextRowCap = 1024;`. The eight non-R1 segments alone reach **629 B** at full mask (prefix 28 + thermal 62 + tof 147 + imu 127 + gamepad 52 + apds 57 + gps 93 + presence 63), so R1 — the last segment — is already truncated today; my earlier "633 B" was wrong. New worst case 629 + 175 = **804 B**. CSV's own pair (`ps_alloc(1024)` / `int remaining = 1024`) becomes `kCsvRowCap`; worst-case row 551 → 684 B, and the full-mask header goes 641 → 735 B, still inside `char first[1024]` in `resolveSessionTarget` (the "~640 B" comment there was accurate and now needs updating to ~735).

The TEXT suffix helper's bound check was six bytes short of its own worst case (`"@~999999"` is 8 chars + NUL):

```c
  if (len + 10 > cap) return;    // 8 suffix chars + NUL + slack
```

**Format coercion — pin at start, never refuse on set.** My original `sensorLogSetFormat` refused any non-CSV while Health Track was on. That silently breaks three shipped flows that fire `sensorlog format track` fire-and-forget and discard the result: [i2csensor_pa1010d.cpp:713](components/hardwareone/i2csensor_pa1010d.cpp:713), [OLED_Mode_Map.cpp:1537](components/hardwareone/OLED_Mode_Map.cpp:1537), [WebPage_Maps.cpp:133](components/hardwareone/WebPage_Maps.cpp:133). With Track on they'd write CSV rows under a track filename, `cmd_gpslog` would report success, and the refusal would also skip the `gSensorLogMask = LOG_GPS` side effect. So:

```c
// Chokepoint keeps ONLY the running-session guard — the one that protects a
// file already committed to a grammar. No Health-Track clause: `sensorlog
// format track` is fired fire-and-forget by gpslog and both Map live-track
// flows, which discard the return value and would silently write CSV.
const char* sensorLogSetFormat(SensorLogFormat fmt) {
  if (gSensorLoggingRunning && fmt != gSensorLogFormat) {
    cliHint("run 'sensorlog stop' first, then set the format and restart");
    return "Error: can't change the log format while a session is running "
           "(rows would stop matching the file already on disk)";
  }
  sensorLogForceFormat(fmt);
  return nullptr;
}
```

and the pin moves to the one place every entry path passes through — `cmd_sensorlog`'s `start` branch, before the target is resolved:

```c
  // Health captures are CSV, ALWAYS. The old coercion only rewrote TRACK, so
  // from a default TEXT config `healthtrack on` filled health-<date>.csv with
  // pipe-delimited prose. At START, not at SET: this covers manual CLI,
  // sensorLogAutoStart and healthTrackRestartWithCurrentMask, and closes the
  // `stop → format text → start` hole a set-path guard leaves open.
  if (gSettings.healthTrackingEnabled && (gSensorLogMask & LOG_R1) &&
      gSensorLogFormat != SENSOR_LOG_CSV) {
    sensorLogForceFormat(SENSOR_LOG_CSV);
  }
```

Ordering note that still holds: in `healthTrackRestartWithCurrentMask` the coercion must precede `shapeSessionPath`, which branches on `gSensorLogFormat` to pick the per-day CSV stem versus a per-session timestamp.

At [System_SensorLogging.cpp:2158](components/hardwareone/System_SensorLogging.cpp:2158) keep the **non-persisting** form — `sensorLogAutoStart` writes no settings today (even the path fixup two lines down assigns `gSettings.sensorLogPath` directly), and routing it through `sensorLogForceFormat` would make it a settings writer on every dark-boot resume:

```c
  if (gSensorLogFormat != SENSOR_LOG_CSV) gSensorLogFormat = SENSOR_LOG_CSV;  // not persisted; see start-path pin
```

`cmd_set_sensorlogformat` at [System_Settings.cpp:2796](components/hardwareone/System_Settings.cpp:2796) still routes its write through the chokepoint *before* `handleSettingCommand` (which persists first, so a later refusal would leave the rejected value to resurrect). Side effect worth naming: that also fixes an existing divergence — today `sensorlogformat 2` sets TRACK without the `gSensorLogMask = LOG_GPS` coercion the CLI applies.

### What else changes

- Six format surfaces attempt this, not four: the two CLI writers, plus [OLED_Mode_Logging.cpp:274](components/hardwareone/OLED_Mode_Logging.cpp:274), [WebPage_Logging.h:711](components/hardwareone/WebPage_Logging.h:711) (a different page from Settings), and the three fire-and-forget map/gps sites. Only the two web pages can surface an error string.
- `OLED_Mode_Logging.cpp` uses **no** `broadcastOutput` today. Label the row `Fmt: TXT [lock]` when a session is running and skip the cycle silently, or use `executeOLEDCommandWithResult` — don't introduce a new output dependency in that TU.
- `healthAgeStr` for `healthstatus` needs its own `#if ENABLE_R1_HEALTH` wrapper; there is no file-scope region there, the guards live inside each function body.
- `buildHealthStatusJson` schema 2 → 3 with `tsSrcMask`. My reason for excluding the absolute stamps was wrong — `CompactJson::kv(const char*, const char*)` exists at [BLE_Events.h:62](components/hardwareone/BLE_Events.h:62) and a snprintf'd decimal ships today. Exclude them instead because JSON consumers would then have two freshness representations to reconcile against `*AgeSec`. The `long`-is-32-bit truncation hazard is real and is why they must not go through the numeric overload.
- `docs/USERGUIDE.md:1071` carries the `sensorlog format` help line; `docs/COMMAND_REFERENCE.md` needs a regen at the end of the batch. `docs/R1_HEALTH_FIXES_PLAN.md` contradicts this in three places (`:316-318` prescribes `_age_s`, `:399` says don't touch the healthstatus basis in the logger, `:536` says Fix 3 extends the `_age_s` pattern) — all three must be rewritten or Fix 3 gets built against them.
- `:540` of that plan says the healthstatus text is a three-way collision to be written once after Fix 3. Rewriting it here is against your own sequencing; either accept Fix 3 re-touching it or defer.
- Version: on-disk format break + JSON schema bump ⇒ 0.100.0, which per RELEASING.md is `PROJECT_VER` plus three doc titles plus a CHANGELOG section.
- The `-2` day-file variant only appears on a no-erase/OTA flash; after a full erase there's nothing to collide with.

### Residual uncertainty

- Within one TEXT row the printed prefix (`getTimestampPrefixMsCached`, read at row-build time) and the ages (derived from `snap.rowStampMs`, captured at snapshot time) are two different clock reads. TEXT/CSV agree with each other; the visible prefix and the suffixes are microseconds apart at best and a full clock step apart at worst. The `@?` branch's correctness argument leans on `rowStampMs`'s basis matching what the reader sees.
- The `@?` case is unreachable in practice — dark host *and* a ring with a valid clock, and `g2RingTimeSyncTick` adopts within ~500 ms. I would not trust that branch until someone has seen it print. The CSV equivalent (magnitude mismatch) is easy to confirm.
- `r1_ts_src` as a mask versus six columns is a genuine judgement call. My pick: mask, because two copies of a bit assignment is the worse failure. But it is the one column a human can't read at a glance and there's no compile-time check tying the bits to the header comment.
- Nothing in the firmware parses R1 CSV columns — only [System_Maps.cpp:2787](components/hardwareone/System_Maps.cpp:2787) reads sensorlog CSV at all, and only for tracks. No in-repo consumer to migrate.

### Size

One session. Six code files plus the doc/version tail. The `R1MetricStamp` addition is the piece I'd budget most carefully — it touches every write site in `ringExtractTelemetryCache` and `g2RingNoteForwardedTelemetry`, and a missed site means a stamp with the wrong provenance bit rather than a compile error.

---

## #3 — The `sRingTsSeen` custody trap and the activity/daily on-ramp

### The resolution

Split this. **Ship the custody funnel now, alone**: `sRingTsSeen` gets exactly one writer, `ringNoteClockWitness(RingClockInstant, src)`, whose `explicit` ctor makes `ringNoteClockWitness(a.dayStartTs, …)` a compile error. Activity/daily must never touch custody — not via `dayStartTs` and not via a derived `baseTs + (highestSlot+1)*600` instant. **Hold the day-total cache**: the all-or-nothing `badRecords` refusal I proposed as its safety property is blind in the one case that matters, and I can't close that from the tree.

The on-ramp answers, when the cache does ship: paced `queueDailyCmd` queue (never the single-slot latch), depth 4 → 8 behind a named cap, policy in `g2HealthCatchupTick()` and a 2000 ms pump in `healthTrackTick()` above the `healthTrackingEnabled` early-out at [System_SensorLogging.cpp:1620](components/hardwareone/System_SensorLogging.cpp:1620), with the 20 s settle checked in **both** the producer and the drain. Day labels via `fmtTrendDayLabel` promoted to `g2HealthFmtDayLabel`, shown only when the ring's UTC window isn't the host's current UTC day.

### Why the symmetric custody write is wrong

`ringBestKnownEpoch` at [G2_Ring.cpp:379](components/hardwareone/G2_Ring.cpp:379) takes `sRingTsSeen` as its *unconditional* baseline; the four point caches override it only when strictly greater. So a midnight `dayStartTs` wins precisely when the point caches are empty or invalid — the dark-boot / unworn-ring state where `ringAdoptClockIfDark` actually fires — and `settimeofday` lands up to 24 h slow, propagating into `rtcSyncFromSystem` + `rtcTimeHasBeenSet`, dated capture folders, `resolvePendingUserCreationTimes`, the automation scheduler, and the corrective `systemTime` push back into the ring. The `endTs` write at [G2_Ring.cpp:687](components/hardwareone/G2_Ring.cpp:687) is legitimate — a window *end* is the ring's clock at its last sample — and that legitimacy is exactly what invites the symmetric line for the activity sibling.

The derived variant is worse, not a compromise. `slot` is a raw wire byte at a speculatively-parsed offset; a 14-byte header (what the python codec claims) shifts every record and a garbage slot of 255 skews the instant **+42.5 h forward**. Backward custody errors are recoverable — NTP corrects them and the drift push repairs the ring. A forward error adopted on a dark boot is written to the DS3231, marks `rtcTimeHasBeenSet`, and is pushed back into the ring as authoritative, with no second source to catch it. It also isn't an observation of the ring's clock: the highest populated bin lags arbitrarily on an unworn ring, and the reply is paginated, so "highest slot seen" is a function of which page arrived. And it buys nothing — `ringRunStandardSetup`'s dark window already solicits `hr/point` and the tick fires up to three more probes at ~setup+0/+5/+10 s, each carrying a real per-sample epoch from the high-confidence 7-byte point layout, long before activity arrives behind a 20 s settle.

### The code

**The funnel** — insert below the declarations at [G2_Ring.cpp:304](components/hardwareone/G2_Ring.cpp:304):

```c
static constexpr uint32_t RING_EPOCH_MAX = 4102444800u;  // 2100-01-01

// A ring epoch naming an INSTANT the ring's clock observed — a sample time, or
// a day-window END coinciding with the last sample. NEVER a window START. The
// ctor is explicit so the obvious symmetry line
//     ringNoteClockWitness(act.dayStartTs, "activity");   // ← will not compile
// is a compile error; forcing it means writing RingClockInstant(...) where a
// reviewer sees the claim being made. A comment was already tried — see the
// existing note on sRingTsSeen — and it is what invited this write.
struct RingClockInstant {
  uint32_t ts;
  explicit RingClockInstant(uint32_t v) : ts(v) {}
};

static void ringNoteClockWitness(RingClockInstant inst, const char* src) {
  const uint32_t ts = inst.ts;
  if (ts == 0) return;
  if (!Clock::isValidEpoch((time_t)ts) || ts >= RING_EPOCH_MAX) {
    static uint32_t sRejects = 0, sRejectLogMs = 0;
    sRejects++;
    if (everyMs(&sRejectLogMs, 10000)) {
      DEBUG_G2F("[RING] clock witness rejected: ts=%lu from %s (outside 2020..2100) "
                "— ring never took our systemTime? (%lu so far)",
                (unsigned long)ts, src ? src : "?", (unsigned long)sRejects);
    }
    return;
  }
  sRingTsSeen = ts; sRingTsSeenRxMs = millis();
}
```

The two existing writes at [G2_Ring.cpp:618](components/hardwareone/G2_Ring.cpp:618) and [G2_Ring.cpp:687](components/hardwareone/G2_Ring.cpp:687) convert to `ringNoteClockWitness(RingClockInstant(ts), "health/point")` and `…(daily.endTs), "health/daily"`. Grep after: exactly one writer and one reader may remain, or the barrier is decorative. `RING_EPOCH_MAX` replaces the inline literal in `ringBestKnownEpoch` — and note `Clock::isValidEpoch` is **floor-only** (`t > 0 && tm_year >= 120`), so the cap is not redundant; on a 32-bit `time_t` build anything ≥ 2³¹ goes negative and the floor catches it, on 64-bit it sails through.

Moving the gate to write time is a small behavioural change: net custody is identical (the read already rejected those values) but rejections are now *visible*, rate-limited to one line per 10 s.

**The gates on `dayStartTs`, with the ordering bug fixed.** My original ran the range check before the record-count test — but [System_R1_Protocol.cpp:812](components/hardwareone/System_R1_Protocol.cpp:812) documents that `base_ts` is **zero for the overview frame**, so that scary "header is not 7 B" warning would fire on every normal reply, and the `count == 0 // overview frame` comment sat after the gate that already swallowed it. Correct order:

```c
  if (!r1ParseActivityDaily(d.payload, d.payloadLength, a)) { /* too short */ return; }
  if (a.count == 0) return;   // overview frame (page 0x04): base_ts is 0, no records — not an error

  if (!Clock::isValidEpoch((time_t)a.dayStartTs) || a.dayStartTs >= RING_EPOCH_MAX) { …refuse… }
  if ((a.dayStartTs % 86400u) != 0) { …refuse… }
```

Be honest about the division of labour there: the 2100 cap rejects only ~4.5% of random u32 — it's a weak filter whose real jobs are arithmetic safety (`4102444800 + 86400 < 2³²`, so `dayStartTs + slot*600` provably can't wrap) and honest labelling (a 2106 stamp would otherwise render as a plausible date). The **modulo test is the strong gate**: a random u32 is a UTC midnight with probability 1/86400.

One correction to *why* that invariant holds. I attributed it to the tz=0 push at [G2_Ring.cpp:1058](components/hardwareone/G2_Ring.cpp:1058). But the pinned fixture at [System_R1_Protocol.cpp:1015](components/hardwareone/System_R1_Protocol.cpp:1015) was captured from the FlutterApp phone app, which pushes the phone's real tz, and its base `0x69F53E80` = 1,777,680,000 = 20575 × 86400 exactly. So UTC-midnight bucketing is a **ring-firmware property** evidenced under a nonzero tz, not a consequence of our push. Document it that way — and drop the derived warning that the gate will false-refuse the day someone changes tz.

**The blocker.** The slot canary is what makes the all-or-nothing refusal protective, and its ~94%-per-frame figure assumes records start at offset 7. [System_R1_Protocol.cpp:814-816](components/hardwareone/System_R1_Protocol.cpp:814) says the final record may be truncated and "the ring continues the data in subsequent notify frames" — that continuation's alignment is a guess, and the fixture's 2 trailing bytes are the only evidence. If a continuation frame resumes mid-record, the parser is shifted 2 bytes and `slot` reads the *high byte of steps*, which is `0x00` for every record in the fixture. `0x00 < 144`, so `badRecords` stays 0, the frame is accepted, and every record collapses into bin 0 — and the accumulator's idempotence ("a record overwrites its slot") actively hides it because they all target the same bin. That is the silent-corruption mode I declared unacceptable, reintroduced through the door I didn't check.

So: keep the annotator (parse-and-log is harmless), and gate only the cache-and-render path — refuse any frame following one with `partialBytes != 0`, and treat "all records `slot == 0` with `steps == 0`" as a shape-change canary in its own right — until a real multi-frame capture settles continuation alignment. `ringquery activity daily` with the G2 debug family on, counting frames per reply, is the experiment.

**The on-ramp, with three corrections.**

Arm from the connect-success path, not inside the setup ritual. `ringRunStandardSetup` has several early `return false` paths, and [G2_Ring.cpp:1470](components/hardwareone/G2_Ring.cpp:1470) **ignores its return value** — it broadcasts "Connected", posts `SYSEVT_RING_CONNECTED` and saves the MAC regardless. A half-failed setup would leave `sLinkUpMs == 0` for that link's entire lifetime, silently. Arm next to `ringPushStatusEvent("connect-ok")` at [G2_Ring.cpp:1474](components/hardwareone/G2_Ring.cpp:1474).

The settle must be in the drain, not just the producer. My `ringLinkSettled()` sat only in `g2HealthCatchupTick`; every other producer of that queue is lens-side (`g2HealthOpen`, `queueTrendsRefreshAll` at [G2_Health.cpp:162](components/hardwareone/G2_Health.cpp:162), `requestTrendDaily`, and the `sDailyRequest` latch that `g2HealthConsumeDailyRequest` at [G2_Health.cpp:560](components/hardwareone/G2_Health.cpp:560) still falls through to) and none respect it. Close Trends mid-drain, or reconnect with leftover queue content, and the pump fires a paginated reply at t+0 on a fresh link — straight into the 14.0 s window. Export `ringLinkSettled()` and put it in the pump's condition too.

The link-up hook writes only the volatile word:

```c
static volatile uint32_t sLinkUpMs = 0;   // written by the BLE connect worker
static uint32_t sSweptForLinkUpMs = 0;    // main loop only
static uint32_t sActivityAskedMs  = 0;    // main loop only

void g2HealthNoteRingLinkUp(void) {       // connect worker context
  const uint32_t now = millis();
  sLinkUpMs = now ? now : 1;              // single-word publish; the tick derives the rest
}
```

`sSweptForLinkUpMs != sLinkUpMs` is "needs sweeping". My earlier version had the worker writing `sCatchupSwept` and `sActivityAskedMs` — plain statics commented "main loop only", contradicted by the function immediately below them.

**Queue depth.** `static uint8_t sDailyQueue[4]` at [G2_Health.cpp:65](components/hardwareone/G2_Health.cpp:65), bounded by `sizeof(sDailyQueue)` at [G2_Health.cpp:158](components/hardwareone/G2_Health.cpp:158) with a **silent** drop. Grow to `kDailyQueueCap = 8` and replace the `sizeof` — it's correct today only because the element type is `uint8_t`, and widening to a struct turns that line into an off-by-8 overflow. Correction to my fill claim: today's max is **3** (`queueTrendsRefreshAll` resets then adds HR/HRV/SpO2; `requestTrendDaily` only re-offers one of those and dedups). 5 is the fill *after* the stale-metric sweep plus activity — there is no present-tense overflow yet.

Also: `queueTrendsRefreshAll` resets `len`/`idx` before adding, so it silently discards the ACTIVITY entry queued at page open, and `sActivityAskedMs` was already stamped — a user who opens Health then immediately taps Trends gets no steps/kcal for ten minutes. Either re-add `R1_CMD_ACTIVITY` there or clear `sActivityAskedMs` on any page-driven queue reset.

**The day label.** `fmtTrendDayLabel` at [G2_Health.cpp:1145](components/hardwareone/G2_Health.cpp:1145) becomes public `g2HealthFmtDayLabel` (one implementation, four surfaces) with a stub in the `!ENABLE_R1_HEALTH` branch. The qualifier renders empty when the ring's window is the host's current UTC day, the label when it isn't:

```c
void g2HealthFmtDayQualifier(char* out, size_t cap, uint32_t dayStartTs) {
  if (!out || !cap) return;
  out[0] = '\0';
  if (dayStartTs == 0) return;                 // forwarded push carries no window — say nothing
  const time_t now = time(nullptr);
  if (Clock::isValidEpoch(now)) {
    const uint32_t n = (uint32_t)now;
    if ((uint32_t)(n - (n % 86400u)) == dayStartTs) return;   // our UTC day — no disclaimer
  }
  g2HealthFmtDayLabel(out, cap, dayStartTs);
}
```

My doc comment claimed a dark host always yields `"boot N"`. It doesn't — `dayStartTs` has already passed the plausibility gate, so the label renders a real date. That output is arguably better (it's the ring's honest date), but fix the comment rather than the code, or pass 0 if `boot N` was actually intended. Never the word "today": the window is a UTC day, so a UTC-8 user's total resets at 16:00 local.

### What else changes

- Buffer sizing on the Overview line: `char act[40]` clips — `"%lu st · %lu kcal (%s)"` with a 15-char `"boot 4294967295"` qualifier reaches ~40 B. Use `act[56]` and `dayq[20]`. `kOverviewTextGeom` is at [G2_Glasses.cpp:18409](components/hardwareone/G2_Glasses.cpp:18409) (not 17358); anchor the `readout`/`lastReadout` widening on `lastReadout[160]` at [G2_Glasses.cpp:18517](components/hardwareone/G2_Glasses.cpp:18517), which is unique in that file.
- `seriesWantsDaily()` does not exist anywhere in the tree — mark it as a placeholder in the sweep comment so it doesn't read as compile-ready.
- BTC_TASK headroom: `sdkconfig` and `sdkconfig.defaults` are **8192**; only `sdkconfig.esp32s3` still says 4096 and `sdkconfig.esp32` says **3072**. The statics-not-locals decision stands (3072 is the stronger argument), but cite the current configs.
- Exclude activity from `freshestVitalAge()` — a 10-minute-cadence day total would dominate the shared `min()`.
- `buildHealthStatusJson` gains steps/kcal/actDayTs/actAgeSec; the `!ENABLE_R1_HEALTH` branch hard-codes `"schema":1` with a different shape, and `/api/health/status` is absent from `docs/APP_JSON_CONTRACT.md` entirely.
- `G2RingTelemetry` has two copies in `G2_Ring.h`; they are currently in lockstep, and both need the five new fields or a no-BT build breaks.
- `docs/R1_HEALTH_FIXES_PLAN.md` Fix 3 prescribes a raw `< 1600000000u` floor and a `baseTs` field name and calls the on-ramp "the least-settled piece" — all superseded.

### Residual uncertainty

- **The pagination question is the blocker, not a caveat.** Continuation-frame alignment is unverified, and in the one misalignment I can reason about the canary is systematically blind. Until a multi-frame capture exists, the day-total cache should not ship.
- `kActivityRefreshMs = 600000` is a judgement, not a measurement. It's a new permanent traffic source on a link with an unexplained 14.0 s death cluster; only an overnight run says whether it costs sessions. Knobs in order: widen the pump 2000 → 5000 ms, raise the refresh interval, then gate activity behind `healthTrackIsActive()`.
- The pump excludes the lens page but **not** `healthTrackTick`'s own traffic: `HT_OD_POLLING` and `HT_MINE_POLLING` each walk a 700 ms vital ladder, and `g2RingPollVitalForLogging` fires from `sensorLogTick` on every sample. A paginated multi-frame reply can land on top of a vital burst. Unmodelled.
- Two independent pacers for one queue — the lens loop's local `lastDailyMs` (700 ms, [G2_Glasses.cpp:18601](components/hardwareone/G2_Glasses.cpp:18601)) and `sHtDailyLastMs` (2000 ms) share no state, so at a page open/close handover the lens pacer's zero stamp fires immediately and two queries can go out milliseconds apart.
- The 2000 ms pacer spaces *requests*, not replies. If one activity query spans more than 2 s of frames, the next daily lands on top of it. Count frames per reply before trusting it.
- `g2HealthConsumeDailyRequest` falls through to the single-slot latch, so the pump also drains page-originated latch requests off-page. Harmless today, but "the latch is excluded from the new path" is not actually enforced.
- Unverified: whether the ring emits empty bins at all. My re-sum is correct either way (missing bins stay 0), but any future "active minutes" derivation from bin count would be wrong.

### Size

The custody funnel alone is under an hour — one file, six lines of type, two call-site conversions, one literal consolidation, and it's a strict safety win with no new surface. The day cache plus on-ramp is a session and a half *after* the pagination capture, spread over the seven files plus doc/protocol updates.

---

## How #2 and #3 interact, and the order

#3's day totals are CSV columns in #2's grammar, so #2's convention has to exist first: `SENSORLOG_EPOCH_MS_FLOOR`, the interleaved `value,stamp` pairing, and the `r1_ts_src` mask. My earlier §8d contradicted this — it prescribed `,r1_steps,r1_steps_age_s,r1_kcal,r1_kcal_age_s` "in Fix 2's interleaved style", but #2 rejects ages in CSV outright. Shipping both as written gives twelve `_ts_ms`-semantics columns beside two `_age_s`-semantics ones: exactly the two-columns-meaning-something-different-from-their-neighbours that #2's own rejection list rules out. That mismatch came from `docs/R1_HEALTH_FIXES_PLAN.md:316-318` and `:536`, which #2 supersedes — rewrite those lines or the next person builds against them again.

Correct form: `,r1_steps,r1_steps_ts_ms,r1_kcal,r1_kcal_ts_ms`, with `G2RING_TSSRC_STEPS (1<<6)` / `G2RING_TSSRC_KCAL (1<<7)` — both structurally clear, like battery and wear, because `actDayTs` is a window start and `actRxMs` is a receive instant. **That exhausts the `uint8_t` mask**, so widen `tsSrcMask` to `uint16_t` in the same edit or document bit 8 as the growth point. Same for the dedup `flags` byte at [System_SensorLogging.cpp:787](components/hardwareone/System_SensorLogging.cpp:787): bits 1..32 are used, steps and kcal fill 64 and 128 exactly, so it must go `uint16_t` or the next field aliases silently.

Landing order:

1. **#3's custody funnel** — independent of everything else, no format implications, strictly safer. Ship it first or in parallel.
2. **#2** — establishes the timestamp convention, the mask, the buffer caps, the format pin. One forced day-file generation.
3. **#3's day cache + on-ramp** — after the pagination capture, extending #2's mask and columns rather than inventing a second freshness representation.

Both #2 and #3 write CSV headers, so landing them separately forces two `-2` variant files instead of one. If the pagination question resolves quickly, folding #3's four columns into #2's header change is worth one combined release; if it drags, take the second variant file rather than blocking #2 on it.

Build both gates before committing either: `ENABLE_BLUETOOTH=0` for #2's unconditional `G2_Ring.h` include, and the `!ENABLE_R1_HEALTH` stub branch for #3's four new hooks. A green FeatherS3 proves nothing about either path.