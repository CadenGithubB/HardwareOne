# File catch-up: logs from a device that was away

A device that is away from home (the portable) keeps writing its logs. When it is back in
range, the home hub pulls the new parts of those logs onto its SD card over ESP-NOW, by itself.
This page says what is sent, how to set it up, and how to see what happened.

Two build switches turn the two sides on:

| Switch | Side | Configuration that has it |
|---|---|---|
| `ENABLE_ESPNOW_SEND` | the device: offers its files | `configurations/p4-portable.json` |
| `ENABLE_ESPNOW_COLLECT` | the hub: pulls them onto its card | `configurations/p4-home-node.json` |

A build has one side, not both. A device without the switch answers "unsupported" once a session
and is skipped.

## What is offered, and how

At most 32 files a device, the newest of each kind:

| Kind | Files | On the air |
|---|---|---|
| `sensor` | sensor captures (`logging_captures/sensors/<day>/...`) | plain CSV; a capture with GPS columns, a TEXT or TRACK capture, or anything that is not a plain sensor CSV is **sealed** |
| `events` | `events.log` | **sealed** (it holds logins and remote command lines) |
| `battery` | `battery.csv` and its two rotations | plain |
| `health` | ring history (`.r1h`) and health CSVs | only when sealed **at rest** (capture encryption); never sent in clear, never withheld silently: counted as withheld |
| `errors` | `errors.log`, `crash-history.log` | plain |
| `system_log` | the system log and its rotations | **sealed** |
| `transcript` | saved transcripts (`/stt/u<id>/*.txt`) | **sealed** |

"Sealed" means each frame is encrypted with the device's capture key (the key that seals health
files at rest) before it leaves; the hub stores it as it came, still sealed, and cannot read it.
Without a capture key the sealed kinds are withheld (counted), never sent in clear.

Never offered: login logs, the command audit, `system-events.log`, QuickNotes, recordings, GPS
tracks, keys, state files and ESP-NOW files.

Who may read: only a master or backup master **named on the device by key**
(`espnowmasterfingerprint`, `espnowbackupfingerprint`), whose firmware is a collector. No
password travels. The backup master gets exactly what the primary gets. A bond worker serves
nobody yet. A byte counts as delivered once **one** named master has stored it; the other master
then skips it and notes it as "at the other master" (a later package syncs the two masters).

## Setting it up

1. On the device: `espnowmasterfingerprint <the hub's key from espnowidentity on the hub>`.
2. On the hub: `espnowmeshrole master` (or backup), a mounted card and a set clock.
3. Pair the two (they need an encrypted session).

That is all: the hub pulls when the device's session comes back, every 15 minutes, after the hub
starts or a card is put in, and on `collectnow`.

## Commands

On the device (admin):

```
espnowcatchup [json] [from <n>]   - the files offered, how far each was taken, who asked last
espnowcatchupserve [on|off]       - serve file catch-up (default on)
```

On the hub (admin):

```
collectstatus [json] [<device>] [from <n>]  - each device: fetching, up to date, waiting and why
collectnow <device> [full]        - pull now; full also fetches what the other master has
collect [on|off]                  - pull on its own (default on); no argument: why it is not pulling
collectrate [<5..100>]            - frames per second the hub asks for (default 40)
collectquota [<64..65535>]        - card space per device in MB (default 2048)
```

## On the hub's card

`/sd/collect/<mac12>/files/`:
- `index.json`: every file (its id, kind, name on the device, the ranges held, the gaps with their
  reason, the ranges "at the other master", digests), times in UTC;
- one or more segments per file, named only from ids (`<ledger>-<file>-s<k>.raw` for plain,
  `.sealed` for sealed records); a device's file name never becomes a path;
- `index-archive.jsonl`: files that left (ledger reset, evicted for the quota, rolled segments).

A gap has a reason: `before_first_fetch` (the device had dropped the start before the first pull),
`trimmed` (the device's capped log dropped it before it was fetched), `lost_on_device` (the
device no longer has the file and a full listing agrees) and `no_longer_offered` (the file left the
device's newest 32; it comes back if the file is offered again).

A new pairing with the same device moves its folder to `files.prev-<UTC stamp>`.

## Limits

- One device is pulled at a time; a pull yields after 64 MB or 30 minutes.
- The device sends at most one frame every 20 ms and only while its send queue is short, so live
  readings, battery and events go first.
- A file is pulled up to 256 MB.
- The P4X-EYE has no battery reading, so no battery floor applies to it; on boards with one, a
  device below 20 % and discharging answers "busy".

Bench steps and the wire layout: FILES_IMPL_SPEC (design folder, 2026-11-19) and
`docs/COMPATIBILITY.md` (CATCHUP_* 82-85).
