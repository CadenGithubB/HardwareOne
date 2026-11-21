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
| `ring panel feed [on\|off]` | (admin) Feed the panel automatically; saved, on by default |
| `ring panel history [minutes]` | (admin) Ring history refresh interval while the feed runs, 10–1440, default 30; saved |

Both settings are also in the settings registry (`g2HealthPanelFeed`, `g2HealthPanelHistoryMin`).

Not sent: the productivity score (it comes from Even's cloud).

## Insights page (`ring insights`)

The expanded Health page has a second page, "insights": up to three lines, each under one of the
glasses' health icons. The Even app fills it with text from Even's cloud; HardwareOne fills it with
ranges from the R1 ring history instead.

### What it shows

Over the most recent `ring insights days` days that have any ring data (1–7, default 3; today
counts when it has data):

| Line | Icon | Text |
|---|---|---|
| 1 | heart rate | `HR 48-142 bpm, avg 69`, a blank row, `HRV 22-95 ms, avg 45` |
| 2 | steps | `Steps 4,200-9,800/day, avg 7,000` over complete days (today is partial and left out) |
| 3 | sleep | `Sleep 6h10-7h40, avg 6h58` from each night's time asleep, two blank rows, `Averages of the past 3 days` |

- Heart rate, HRV and SpO2 come from the ring's hourly buckets: the low is the lowest hourly
  minimum, the high the highest hourly maximum, the average the mean of the hourly averages (each
  hour counts once). Buckets outside 30–220 bpm, 1–300 ms or 70–100 % are left out.
- A note ends the last line, under two blank rows, so it stays out of the data:
  `Averages of the past 3 days` (`... past 2 days` when only two exist, `... past day`), or
  `So far today` when today is the only one. It goes on line 3;
  without a line 3 it ends the last line there is. It is left out (never the data cut) if that line
  would pass 255 bytes.
- Without HRV, line 1 is heart rate only; with HRV but no heart rate it is HRV under the HRV icon.
- Only today has steps: `Steps today 3,100`. One value only: `Steps 6,400/day`, `Sleep 7h05`.
- No sleep in the window: line 3 is `SpO2 92-99%, avg 96` under the SpO2 icon.
- A line without data is left out; with no ring data at all the glasses show their own
  "gathering insights".
- The glasses add a fixed footer, "For daily reference only. Not medical advice.".

### Wire

sid 0x0E to the right temple, flag 0x20, the shape the Even app sends (captured on 2.3.2.14):
`{1 cmd=4, 2 magic, 6 {1 item {1 type, 2 text, 3 0} (1 to 3 times), 2 0, 3 1}}`, or, with
nothing to show, `{1 4, 2 magic, 6 {2 0, 3 3}}`. The glasses answer `{1 4, 2 magic, 6 {1 {1 type}
per stored line, 3 status}}`. Rules the encoder (`G2_HealthInsightsCore.h`) keeps:

- at most three items: the glasses reject a message with four (no echo);
- every length is a protobuf varint, two bytes from 128 up (a one-byte length is dropped);
- text is ASCII (`-`, not an en dash), at most 255 bytes, no NUL; a newline starts a new row
  under the same icon, and a row with one space gives the gap in line 1;
- icon types 2 steps, 3 calories, 4 sleep, 5 heart rate, 6 SpO2, 7 temperature, 8 HRV; others are
  not drawn and never sent;
- the magic comes from the same counter as the snapshot above, so two sid 0x0E messages in a row
  never share one.

A strict parser re-reads the bytes before the write, in `G2_HealthInsights.cpp` and again at the wire
in `G2_Glasses.cpp`. The glasses keep the lines in RAM only; each message replaces all of them, and
the Even app overwrites them when it connects.

### Timing

- once 5 s after the right temple is ready (the snapshot goes at 2 s, and insights wait until its
  echo is in), then when the lines change, at most once a minute;
- one retry if no echo arrives within 3 s, then nothing until the lines change;
- paused while Even AI, Conversate, the native terminal or a note is active;
- only while `ring panel feed` is on and `ring insights feed` is on (both default on);
- nothing is sent before the ring has delivered a history day in this boot: the ring's identity
  and day are not known until then.

### Where the work runs

The main loop (`g2HealthInsightsMainTick`, from `g2HealthPushMainTick`) re-reads today's day from
the in-RAM history every 30 s while the right temple is connected, and searches the history store
for the older days (up to 14 calendar days back, one day per pass, an hour either way for a
daylight-saving change) whenever a history day was saved, the day changed, the ring's latest
history day moved or the ring changed. That search is read-only: it never changes the store's
reported status. The day in RAM counts once (a stored copy of it is dropped), and today's copy,
whichever it is, is the partial day whose steps are left out of the range. It
builds the message body and publishes it in PSRAM. The G2 glasses task only adds the magic, writes
it and matches the echo; it never takes the history lock.

### Commands

| Command | Effect |
|---|---|
| `ring insights status` | Feed state, window, current lines, last send and echo |
| `ring insights preview` | The lines and the exact message bytes (shown with magic 1); nothing is sent |
| `ring insights send` | (admin) Send the current lines now; lines older than about 45 s (the feed is off, so nothing refreshed them) are recomputed first: run it again a few seconds later |
| `ring insights feed [on\|off]` | (admin) Send automatically; saved, on by default, runs only with `ring panel feed` |
| `ring insights days [1-7]` | (admin) Days in the window, default 3; saved |

Settings: `g2HealthInsightsFeed`, `g2HealthInsightsDays`.
