# HardwareOne Showcase

A live, narrated view of what a running device is doing, for demos. It is a
laptop-side program only: **nothing on the device changes**. It reads the
device's existing web API (or the USB console) and serves a presentation page
on `localhost` that turns state changes into a story as they happen.

```sh
python3 tools/showcase/showcase.py --host 192.168.1.42 --user admin
```

Then open the printed URL (default `http://localhost:8787/`).

- `--serial /dev/cu.usbmodemXXXX` uses the USB console instead of Wi-Fi. This
  needs pyserial, so run it with the ESP-IDF interpreter
  (`~/.espressif/python_env/*/bin/python3`). The port is opened without
  resetting the chip. The web endpoints are emulated with the matching
  `<cmd> json` commands plus `g2status` for the glasses. Validated on the
  P4X-EYE bench device 2026-10-29. While it runs the port is held, so the
  device's own console is unavailable; the showcase's own polling commands
  are hidden from the console card. Every poll is a command on the device,
  which shows up as `[LOOPHEALTH] stall ... INPUT` lines on builds with
  that debug on; raise `--interval` if that matters.
- The username is the device account name, not "admin" by default on every
  device. A rejected login is reported as such; five failures lock the serial
  login for 30 s.
- Speech pipeline activity (capture start, segments heard, speaker
  recognised) is read from the ESP-IDF `STT: TL` log lines on the console and
  narrated in the feed; the per-second level lines are dropped.
- `--mock` rehearses the page with a fake device (no hardware).
- `--camera` also pulls one JPEG frame a second when the camera is on.
- `--interval 2` is the fast-lane poll period. Per-task CPU shares come from
  the device's shared sampler (the same figures as its OLED Perf page), read
  every poll; nothing on the device blocks to measure.
- `--bind 0.0.0.0` makes the page reachable from another machine (for a
  projector box). The default binds localhost only.
- The password comes from `--password`, the `HW1_PASSWORD` environment variable,
  or an interactive prompt. The user needs to be able to run commands, so not a
  guest account. `power json` is admin-only; a non-admin user just sees the
  processor card without the clock figures.

## On the page

- **Headline** – the latest change on the device, timestamped, with a one-line
  "right now" summary beneath it.
- **Cards** – processor (clock, power mode, loop health, per-task CPU), memory
  (DRAM with low-water mark, PSRAM), radios, wearables (G2, ring, phone link),
  senses (I²C bus, sensors live), power, the story feed, the console mirror,
  tasks and storage, and the camera when it is streaming.
- **Tour** – press `T`. The cards are spotlighted one at a time with a caption
  that explains the subsystem using the live numbers. Captions are templates:
  each step has an intro plus ranked observations with conditions, filled
  from the live state and refreshed every few seconds, so the text follows
  what the device is doing (see `TOUR_TEMPLATES` in showcase.html). `←`/`→`
  step, `Esc` ends, `F` is fullscreen. Clicking a card jumps the tour to it.

## How it talks to the device

- **HTTP Basic Auth** on every request. It is stateless on the device, so it
  never takes one of the two web session slots and never signs your own browser
  out, unlike a cookie login.
- Polled: `/api/system`, `/api/power/status`, `/api/battery/status`,
  `/api/ble/status`, `/api/sensors/status`, `/api/cli/logs`,
  `events` (with `capture=1`) and a `/api/cli/batch` of `memreport json`,
  `perftop json`, `taskstats json` and `espnowmeshstatus`.
- The device's web server is single-task, so all polling is serial from one
  thread, with the 50 ms command spacing respected.
- Story sentences come from two sources: the typed system-event ring (`events`)
  and deltas in the polled state (clock steps, RSSI bucket, heap bucket, sensor
  start/stop, link up/down). Unknown event kinds are shown with their name
  humanised, so new kinds in the firmware still appear.

## What it shows and does not show

The page shows the device's state, the system event ring and the console
mirror (the device already redacts passwords and session ids from that
mirror). It never sends a command that changes anything: `events`,
`memreport`, `perftop`, `taskstats` and `espnowmeshstatus` are all read-only.
Credentials never appear in the page or in URLs.
