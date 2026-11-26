# Troubleshooting

What to look at when a device misbehaves, and what to collect before you report it. Known
problems and their workarounds are in [KNOWN_ISSUES.md](KNOWN_ISSUES.md); every command is in
the [Command Reference](COMMAND_REFERENCE.md). Report a security problem privately, as
[SECURITY.md](../SECURITY.md) describes, never in a public issue.

Every command below runs on the serial console and the web CLI page. Most take `json` for a
machine-readable reply.

## First look

| Command | Shows |
|---|---|
| `versions` | The firmware version and every version a separately built part must match |
| `status` | Wi-Fi, filesystem and memory in one reply |
| `uptime` | Time since the last start |
| `events` | The recent system events (logins, Wi-Fi, battery, mesh changes) |

## After an unexpected restart

| Command | Shows |
|---|---|
| `bootcount` | Boot count, crash count and the last reset reason |
| `crashlog` | The last recorded crash: panic text, core and PC, boot phase, how often it repeated |

The device also keeps logs under `/system/sys_logs/` (admins read them with `fileview` or the web
file browser): `crash-history.log`, `events.log`, `errors.log`, and the boot debug files
`boot-debug-1.log` to `boot-debug-3.log` for the last three boots. When flash runs low, logs move to
the SD card (`/sd/system/sys_logs/`).

## Slow, stuck or short of memory

| Command | Shows |
|---|---|
| `memreport` | Internal and PSRAM heap use and the largest free blocks. The text form prints on the serial console; `memreport json` answers anywhere |
| `taskstats` | Every task with its state, priority and lowest free stack |
| `perftop` | Main-loop timing, the worst stalls, per-task CPU use and who holds the filesystem lock |

## Updates

On a recovery-OTA build, `otastatus` shows the signed update state (`docs/OTA_DEVICE_SETUP.md`).
On other builds it says that OTA recovery is not in the build.

## Reading the serial console without restarting the board

Opening the port with a flashing tool restarts the board: every `esptool` command and a plain
`idf.py monitor` reset it on purpose. To watch a board that is in the state you want to see, open
the monitor with `--no-reset` and the board's build folder (for the crash decoder):

```sh
idf.py -B build-<board> -p <port> monitor --no-reset
```

The console runs at 115200 baud. Log in on it as usual. The monitor decodes a crash backtrace with
that build folder's image, so the decode is right only when that image is the one on the board.

## What to put in a report

The output of `versions`, `bootcount json` and `crashlog json`, the board and saved configuration
you built (`BUILD_INFO.md` in the build folder), what you did just before, and the serial console
around the problem if you have it. Remove passwords, Wi-Fi names and addresses first.
