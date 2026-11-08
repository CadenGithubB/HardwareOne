# G2 Health panel feed (`ring panel`)

HardwareOne can fill the G2's own Health panel with R1 ring data: the dashboard Health tile and
its expanded page. Without a phone, the Even app is what normally does this.

## How the stock glasses get health data

The panel is **not** fed by the ring. The phone reads the ring, then sends the glasses a
`HealthDataPackage` protobuf on **sid 0x0E** (`UI_HEALTH_APP_ID`) to the **right** temple. The
glasses keep the last one in RAM and draw it. The app sends one shortly after connecting and again
whenever its numbers change. A host that reads the ring but never sends this message leaves the
panel showing whatever the Even app last sent.

The ring-to-glasses link (the ring's "glasses" role) carries gestures only and is not involved.

## What a write can affect

Stock firmware behaviour, from the decompiled stock handler and from the app's own traffic:

- **RAM only.** One health record is written. Nothing goes to flash, settings or the dashboard
  layout. A glasses reboot clears it, and the Even app's next push replaces it.
- **Fixed-size decode.** At most 8 entries and short highlight texts. Anything larger fails to
  decode and is dropped without an echo.
- **Light validation.** The glasses check only the entry type (2–9). Goal, value, average and
  duration are stored as sent and cast to integers for display. A sender therefore has to keep
  them finite and in range itself.
- **Echo.** On success the right temple echoes `{commandId, magic, entry types}`.

## What HardwareOne sends

`G2_HealthPushCore.h` builds exactly one message shape: the app's dashboard snapshot (`MULT_DATA`).
It has six entries, in the app's order, with every field written as the app writes it:

| Entry | value | avg | goal | duration |
|---|---|---|---|---|
| STEPS | today's steps (0–100 000) | – | 10 000 | – |
| CALORIES | today's active kcal (0–20 000) | – | 300 | – |
| SLEEP | HardwareOne's sleep score (1–100; 0 = no record) | – | – | asleep seconds (≤ 86 400) |
| HEART_RATE | latest bpm (30–220) | today's mean | – | – |
| BLOOD_OXYGEN | latest % (70–100) | today's mean | – | – |
| PRODUCTIVITY | 0 | – | – | – |

Out-of-range or unknown values become 0, which the glasses show as "-". Before any write, a strict
parser re-reads the encoded bytes and refuses anything other than this shape and these ranges. The
check runs twice: in `G2_HealthPush.cpp` and again at the wire in `G2_Glasses.cpp`.

## Sleep score

The glasses draw a sleep score of 0 as "-" and then hide the duration as well, so a night only
shows with a non-zero score. The Even app shows a score that Even's backend computes from
duration, efficiency and sleep-stage balance; that formula is not in the app. HardwareOne computes
its own from the ring's last night (asleep time and the awake/REM/light/deep stage totals), out of
100:

| Part | Points | Full marks | Zero |
|---|---|---|---|
| Duration | 50 | 7–9 h asleep; −5 per hour past 9 h, never below 35 | 2 h or less |
| Efficiency: asleep ÷ (asleep + awake) | 25 | 90 % or more | 65 % or less |
| Deep sleep share | 12 | 13–23 % of time asleep | 0 %, or 46 % and up |
| REM share | 13 | 18–25 % of time asleep | 0 %, or 50 % and up |

A night without stage data gets 15 efficiency and 12 stage points. Any real night scores at least
1. The score will not match the Even app's number for the same night.

## Timing

Pushes to the glasses:

- one push 2 s after the right temple is ready, but only once HardwareOne has some ring data. A
  snapshot with nothing from the ring would blank every row, so it is never sent automatically (after
  a boot the glasses usually connect before the ring); the panel keeps what it last showed;
- then at most one per minute, and only when a value changed;
- one retry if no echo arrives within 3 s, then nothing until the data changes;
- pushes pause while Even AI, Conversate, the native terminal or a note is active.

Ring data the feed keeps fresh while it is on and the right temple is connected:

- **vitals** (heart rate, SpO2): the health logger's timed read runs even with health logging off.
  There is one ring read and one interval for both: `healthlogging interval` seconds (default
  15 min), so changing it changes the panel's cadence too. Readings go to a file only when health
  logging is on, and only health logging asks Bluetooth to look for a missing ring; with the feed
  alone, the ring's own reconnect settings decide;
- **steps, calories, sleep**: a normal ring history refresh 15 s after both the right temple and the
  ring are connected, then every `ring panel history` minutes (default 30). It queues behind other
  ring work and keeps the history freshness rules. Nothing is requested while the ring is down; if the
  ring refuses (for example with its health collection off) the feed asks again after 2 min;
- opening a Health page on the glasses, the OLED or the web refreshes the data too, and the panel
  follows within a minute.

## Where the work runs

| Task | Does |
|---|---|
| Main loop (`g2HealthPushMainTick`, from the health logger's tick) | Rebuilds the snapshot from the history and live ring data every 10 s while the right temple is connected; schedules the ring history refresh, holding it back while the vitals read is in flight |
| G2 glasses task (`g2HealthPushTick`) | Watches the right-temple link, applies the send rules, writes the message and matches the glasses' echo. It never takes the history lock or starts ring work |
| Ring task | Runs the vitals reads and history sweeps it is asked for, one at a time |

## Commands

| Command | Effect |
|---|---|
| `ring panel status` | Feed state, current values, last send, echo count |
| `ring panel preview [test]` | The snapshot and its exact bytes; nothing is sent |
| `ring panel send [test]` | (admin) Send one snapshot now; `test` uses fixed values: 1234 steps, 210 kcal, 72 bpm, 98 %, sleep score 80 with 7 h 30 min |
| `ring panel feed [on\|off]` | (admin) Feed the panel automatically; saved, off by default. `ring panel on` / `off` are shorthands |
| `ring panel history [minutes]` | (admin) Ring history refresh interval while the feed runs, 10–1440, default 30; saved |

Both settings are also in the settings registry (`g2HealthPanelFeed`, `g2HealthPanelHistoryMin`).

Not sent: health highlights (insight text) and the productivity score. Both come from Even's cloud.
