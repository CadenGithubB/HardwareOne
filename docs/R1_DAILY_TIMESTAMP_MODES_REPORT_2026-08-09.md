# R1 daily timestamp modes: investigation and pre-flash fix

Date: 2026-08-09  
Scope: R1 firmware `2.2.7.0005`, HardwareOne daily HR/HRV/SpO2/activity ingestion

## Outcome

The R1 daily layout does not have one universal timestamp interpretation.
The day key and the latest-sample field must be interpreted independently.
Within one recent fetch the ring used all of the following:

- a zero day base with a seconds-within-day latest value (HR and HRV);
- a zero day base with an absolute latest epoch (SpO2);
- an anchored day base with a seconds-within-day latest value (HR and HRV);
- an anchored day base with an absolute latest epoch (SpO2);
- zero-base and anchored activity pages, which have slots but no latest field.

The safe fix is now implemented. HardwareOne retains raw fields and classifies
their modes, but only emits absolute bucket timestamps when the same page
contains a plausible timezone-aligned day boundary. It does not use the ESP32
clock, receive time, or a neighboring page to manufacture a missing day.

## Evidence

### Latest HardwareOne fetch (`2.2.7.0005`)

The relevant pages arrived in pairs. Health values are intentionally omitted
from this report.

| Metric/page | Day field | Latest raw | Classification | Safe normalized latest |
|---|---:|---:|---|---:|
| HR, first | `0` | `39270` | seconds within day (`10:54:30`) | none: day is unanchored |
| HR, second | `1786233600` | `39270` | seconds within day | `1786272870` (`2026-08-09 10:54:30Z`) |
| HRV, first | `0` | `36067` | seconds within day (`10:01:07`) | none: day is unanchored |
| HRV, second | `1786233600` | `36067` | seconds within day | `1786269667` (`2026-08-09 10:01:07Z`) |
| SpO2, first | `0` | `1786278249` | absolute epoch | `1786278249` |
| SpO2, second | `1786233600` | `1786278249` | absolute epoch | `1786278249` |
| Activity, first | `0` | not present | zero-base slots | none |
| Activity, second | `1786233600` | not present | anchored slots | per-slot epochs |

All of these frames passed the envelope, CRC32, opcode, count-derived length,
slot, and value-range checks. Their final opaque `u32` was `0x00009AD4`; that
field is unrelated to timestamp interpretation.

### Earlier captures

The older evidence prevents us from tying a single mode to a firmware version:

- Official-app sessions on `2.2.7.0005` on 2026-08-03 used anchored day bases
  and absolute latest epochs.
- A 2026-07-31 session on `2.2.6.0009` used zero-base first pages with absolute
  latest epochs, followed by anchored local-day pages.
- The `2.2.7.0005` binary contains separate `*_sync` log strings for RAM,
  flash, `today_zero`, day keys, and maximum timestamps. This supports treating
  zero-base pages as a distinct ring-owned synchronization form, but strings
  alone do not prove that those records can be safely rebased.

Therefore neither firmware version, page order, metric, nor the final opaque
word is a sufficient timestamp-mode discriminator by itself.

## Contributing HardwareOne bug: timezone forced to UTC

Official-app `systemTime` captures consistently send a signed offset in minutes
(`-240` for the archived EDT sessions). The R1 builder's existing golden vector
also encodes `-240`. HardwareOne's transaction owner nevertheless called the
builder with a literal zero.

The most recent setup therefore told the ring to use UTC, and the anchored
pages came back with a UTC-midnight day key and timezone zero. HardwareOne now
sends `Clock::tzOffsetMinutes()` after checking the supported `-720..840`
range. This is a direct protocol field, not a date or age calculation.

## Implemented safety model

### Day base

Each parsed result now carries one of:

- `zero-base`: raw day is exactly zero;
- `epoch`: raw day is in `[2020-01-01, 2100-01-01)` and becomes local midnight
  after applying the page's signed timezone offset;
- `unknown`: nonzero but not proven to be a valid day boundary.

The raw `dayStart` is always retained. Records on zero-base or unknown pages
retain their slot and values but receive `bucketEpoch=0`.

### Latest timestamp

Common and HRV pages retain both `latestTimestampRaw` and a mode:

- `none`: raw value is zero;
- `epoch`: raw value itself is a plausible absolute epoch;
- `seconds-within-day`: raw value is less than 86400;
- `unknown`: any other nonzero shape.

`latestTimestamp` now means normalized absolute epoch only. A
seconds-within-day value is normalized only when its own page has an anchored
day. Otherwise it remains zero while the raw value and mode remain available
for diagnostics.

### Downstream gates

- Daily packet ACK behavior is unchanged: a structurally valid zero-base page
  is ACKed so ring flow control continues.
- Log annotations print raw day/latest values, their modes, and the normalized
  latest epoch.
- Immediate trends and live backfill require `dayMode=epoch`.
- Typed history merge requires `dayMode=epoch` and the existing exact
  `{peer, profile, dayStart, timezone}` key.
- The persistent history store continues to accept only nonzero exact day keys
  and exact derived bucket epochs.
- Zero-base pages are not merged with a following anchored page. That would be
  an inference not yet established by the wire evidence.

## Regression coverage

Boot-time protocol self-tests now include sanitized payloads for:

- zero-base + seconds-within-day (no normalized latest, no bucket epochs);
- anchored day + seconds-within-day (checked normalization);
- zero-base + absolute latest (absolute latest retained, buckets unanchored);
- zero-base activity slots (no bucket epochs);
- a plausible but non-boundary day (classified unknown);
- a latest value that is neither seconds-within-day nor a plausible epoch
  (raw-only unknown);
- the observed changing opaque final word;
- rejection of an out-of-range `systemTime` timezone without consuming a
  protocol serial.

An ESP32-S3 firmware build completed successfully after these changes.

## Residual uncertainty and next capture

The unanchored pages may be RAM/today overview pages, but that is not yet enough
to assign them a date. The next hardware test should verify behavior, not add a
new heuristic:

1. Connect after HardwareOne has a valid clock and the configured local offset.
2. Confirm the `systemTime` write uses that offset.
3. Fetch history and confirm annotations show the expected day/latest modes.
4. Confirm only anchored pages reach trends/history and no 1970-era timestamps
   appear.
5. Repeat across local midnight if practical. A controlled midnight capture is
   the strongest way to determine whether zero-base RAM pages roll over with
   the anchored local-day page.

Until that evidence exists, zero-base records remain intentionally
non-ingestible. This loses uncertain records rather than corrupting durable
history with a guessed date.
