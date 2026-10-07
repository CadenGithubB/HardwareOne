# QuickNote and the glasses hold

QuickNote records a short private note by voice and saves it as text on
HardwareOne. You hold the R1 ring while a HardwareOne page is on the G2 lens,
speak, and let go; the note is transcribed and saved under your own account.
The same note can be started from the lens Notes app or from the `note`
command.

The ring or temple hold ("long-press") is a general HardwareOne input. QuickNote
is its first user: a page may claim the hold for itself, and the global hold
action (`g2holdaction`) decides what a hold does on every other page.

Requirements:

- `ENABLE_QUICKNOTE`, which defaults on wherever dictation, the G2 glasses and
  Bluetooth are built. The P4X-EYE profile sets it explicitly. A board or
  deployment may set it to 0 to save flash; the hold input itself stays.
- For holds: G2 glasses connected, with a glasses firmware HardwareOne has a
  hold profile for (see [Glasses firmware](#glasses-firmware-and-the-even-app)).
- A signed-in, non-guest account that owns the glasses session (the paired
  wearer) for anything started from the glasses.
- A transcription backend: on-device STT on the P4X-EYE, or a signed-in
  Raspberry Pi co-processor on CM5 builds (see
  [P4X-EYE and CM5 builds](#p4x-eye-and-cm5-builds)).

## Starting a note from the glasses

A hold reaches HardwareOne only while one of its own pages is on the lens, so
open Blocks first (the HardwareOne lens UI). Then use any of these:

| Way | What you do | What happens |
|---|---|---|
| Hold on any HardwareOne page | Hold the ring, speak, let go | With `g2holdaction 1` (the default) the lens switches to Notes and shows `● REC 0:07 · release to save`. Letting go stops and saves the note |
| Hold on the Notes page | Apps -> Notes, then hold the ring, speak, let go | The same, without the page switch |
| Notes -> New note | Apps -> Notes, tap `● New note`, speak, tap it again | A tapped note runs until you tap again, run `note stop`, or reach `notemaxsec` |

`speak now` appears under the timer once the glasses microphone is delivering
audio. Start speaking then: anything said before it is not captured.

After a note ends the header shows `Saving…`, then `✓ Saved · 9 words` with the
first 150 characters, or `Not saved: <reason>` / `Failed: <reason>`, for five
seconds.

The Notes list shows your six newest notes as `time title` rows (the title is
the first words of the note), with `More…` for older ones. The time is the
local `hh:mm` the note was saved. A note saved while HardwareOne had no clock
(see [Where notes are stored](#where-notes-are-stored)) shows how long ago it
was saved instead, such as `just now`, `5m ago` or `2h ago`, or `earlier` if it
was saved before the last restart. The time is worked out when the list is
loaded: opening the page, after each note, and on `More…`. Tap a note for
`Read` and `Delete`; Delete needs a second tap within 4 seconds. The reader
pages through the note; double tap returns.

A hold that is released before the microphone starts, or with less than
0.6 seconds of audio, is discarded: no file and no event, and the Notes page
says `Not saved` and why. So is a take that holds nothing but a breath or a
filler word ("um", "okay", "thank you" in under 2.5 seconds).

## Starting a note from a command

`note` is a regular command. Replies are JSON and are marked private, so logs,
the web mirror and other sessions show only `[private STT result]`.

```
note start [hold|toggle] [mic=auto|g2|pdm]   - Start a note (lens, web CLI or local display only)
note start glasses [hold|toggle]             - Start a note on the paired wearer's glasses (any other console)
note stop <id>                               - Finish and save the note
note cancel <id>                             - Discard the note
note status [id]                             - The live note, or how note <id> ended (never its text)
note list [internal|sd] [offset] [u<id>]     - Eight notes per page, newest first, each with its "when" label
note read "<path>" [offset]                  - 512-byte window of a note file
note delete "<path>"                         - Delete a note file
```

Who may start a note:

- `note start` works only where the session can hear its owner: the lens
  (the Notes page runs it as the wearer), a signed-in web CLI session, or the
  local display. From serial, the BLE app or UART it answers
  `From this console use: note start glasses`.
- `note start glasses` works from any signed-in console, but only for the
  account that is the paired G2 wearer. There is no admin override: an admin
  cannot open someone else's glasses microphone. The note records on the
  glasses, belongs to the wearer and runs under the wearer's glasses session.
- Guests cannot record notes. Automations, ESP-NOW, MQTT and voice commands
  can never start one.
- `note stop`, `note cancel` and `note status <id>` accept the note's owner or
  an admin. `note list`, `note read` and `note delete` follow the transcript
  file rules: your own notes, or anyone's for a superadmin (`u<id>` lists
  another account's notes).

`note start` without `glasses` picks the microphone from `mic=` or the `notemic`
setting. Auto uses the glasses when you are the paired wearer and the glasses
microphone is available, otherwise the board microphone. Only the paired
wearer can record on the glasses, whichever console asks. `hold` starts a note
the way a hold does: its trigger is `hold`, a stop with less than 0.6 seconds
of audio discards it, and on CM5 builds the recorder waits longer through
pauses. `toggle` (the default) is an ordinary note.

A typical serial session:

```
login alice <password>
note start glasses
  {"id":"6f1c9a2e00000007","phase":"starting","active":true,...}
note stop 6f1c9a2e00000007
note status 6f1c9a2e00000007
  {"id":"6f1c9a2e00000007","phase":"idle","active":false,"outcome":"saved","reason":"",
   "path":"/sd/stt/u3/notes/20261005T153012Z-6f1c9a2e00000007.txt","words":9,...}
note read "/sd/stt/u3/notes/20261005T153012Z-6f1c9a2e00000007.txt"
```

Only one note records at a time, and a note cannot start while Transcription,
keyboard dictation, Even AI or Conversate is using the microphone.

## What a hold can and cannot do

The glasses send HardwareOne a hold as two events: one when the hold is
recognised and one when the finger lifts. On the bench (P4X-EYE, G2
2.3.2.14 on both arms, 2026-10-05, logged with the firmware before this
feature) every plain hold on a HardwareOne list page delivered exactly one of
each, from the right arm only, with no menu or overlay in between, and the
glasses named the source: right temple, ring or left temple. For holds of
about 2 to 3 seconds the two events arrived 2.4 to 3.8 seconds apart.

A hold can:

- start a note on any HardwareOne page, or record on the Notes page;
- end the note it started when you let go, or when the hold ends any other
  way (the firmware menu or a notification covers the page, the page exits,
  Bluetooth drops). The note is then finished and saved with what was said;
- keep the HardwareOne page alive. Holds count as activity, and a running note
  holds the page open, so the 60-second safety timeout does not close Blocks
  under you;
- trigger automations on boards that build them (not the P4X-EYE profile):
  `g2_long_press` and `g2_long_release` are ordinary events
  (`type=event on=g2_long_press`). An automation still cannot start a note.

A hold cannot:

- start anything when no HardwareOne page is on the lens: not from the idle
  or dark lens, the dashboard, or a native app such as Even AI. The glasses
  give a plain hold to whichever app is in front;
- replace the glasses' own gestures. Tap then hold (the glasses menu), holding
  both temples (silent mode) and double tap (back/exit) stay with the
  glasses;
- act while the glasses own the screen or input: the firmware menu or a
  notification (and half a second after it closes), the keyboard, the camera
  stream or an image probe on the lens, or a page change in progress;
- leave a page that cannot be left: Maps, Health, the LLM viewer and the image
  and camera viewers refuse a hold (`page-busy`);
- start a note when one cannot start now: another note or dictation running,
  Even AI or Conversate active, storage full, or no signed-in paired wearer.
  The page you are on stays as it is.

A hold that does nothing shows nothing on the lens. `g2hold status` and
`events` say why (`ignored:<reason>` or `refused:<reason>`).

### Ring or temples

With `g2holdsource 0` (the default) only a ring hold starts the hold action;
temple holds are recorded with the reason `source` and ignored. Set
`g2holdsource 1` to let the temples start notes too. This only works on
firmware that names the source (2.3.2.14). On 2.2.9.22 every hold arrives as
"unknown", so any hold is accepted whatever `g2holdsource` says.

## Settings

| Setting | Values | Default | What it does |
|---|---|---|---|
| `g2holdaction` | `0` Off, `1` QuickNote | 1 where QuickNote is built, else 0 | What a hold does on a HardwareOne page with no hold handler of its own. A page that handles holds (Notes) always gets the hold first |
| `g2holdsource` | `0` Ring only, `1` Ring + temples | 0 | Which holds start the hold action, on firmware that reports the source |
| `g2holdunprofiled` | `0`, `1` | 0 | Testing only: act on holds from glasses firmware without a hold profile |
| `notemaxsec` | 10 to 120 seconds | 60 on the P4X-EYE, 30 on CM5 builds | The longest note. CM5 builds cap it at 30 |
| `notemic` | `0` Auto, `1` Glasses, `2` Board mic | 0 | Microphone for `note start` from the web or local display. Notes started from the glasses always use the glasses |

Each setting is an admin command of the same name (`g2holdsource 1`); with no
value it shows the current one. They belong to the settings modules `g2hold`
and `quicknote`. They are HardwareOne settings only: nothing about holds is
ever written to the glasses.

## Where notes are stored

Each note is one UTF-8 text file in your own folder:

```
/sd/stt/u<account id>/notes/<yyyymmddThhmmssZ>-<id>.txt   (SD card, when writable)
/stt/u<account id>/notes/<yyyymmddThhmmssZ>-<id>.txt      (internal flash otherwise)
```

The time is UTC, or `boot` when the clock was not set. The file starts with a
short header (`HardwareOne note v1`, id, created, boot, uptime, trigger
`hold|lens|cli|web`, mic `g2|pdm`, duration, words, title), a blank line, then
the text. `Boot:` is HardwareOne's boot counter (the `boot #N` in the boot log)
and `Uptime-s:` the seconds since that boot when the note was saved. Notes saved
before these two lines existed have neither and still read normally. A note
holds at most 4096 bytes of text.

HardwareOne has no clock until something sets it: Wi-Fi time, the RTC, the
R1 ring, the CM5 carrier or `timeset`. On a P4X-EYE with Wi-Fi off and the ring
not connected, notes are saved as `boot-<id>.txt`. If the clock is set later in
the same boot, HardwareOne gives each of those notes its real time: it writes
a copy named `<time>-<id>.txt` with that time in `Created`, checks it, and only
then deletes the `boot-` file. At most 16 notes per boot are fixed this way. A
note from an earlier boot keeps its `boot-` name, because its time can no
longer be worked out. If deleting the `boot-` file fails, a later pass in the
same boot deletes it. If the power is cut between the copy and the delete,
both files stay and both are listed; nothing is lost, and you can delete the
`boot-` one.

Lists are sorted by file name in reverse, which puts timed notes newest
first. The id of a note started without a clock begins with the boot counter
and the seconds since that boot, so `boot-` notes are newest first too, the
latest boot's on top. Every `boot-` note sorts above every timed note, so a
`boot-` note from an earlier boot stays above notes saved later with the clock
set; delete it once it is no longer wanted. `boot-` notes saved by QuickNote
test builds before this ordering have random ids: they list above the others
in no particular order.

A note starts only if the storage it would use has room for a full note above
a 100 KB reserve; nothing is ever deleted to make room. Notes are kept
separately from transcripts: `sttsavetranscripts` does not apply, no
transcript copy is made, and `transcripts list` does not show them.

## Privacy

- The note text exists in a memory buffer while it records (cleared after
  every note), in the saved file, on your lens, and in your own replies to
  `note read` and `note list` (a title is the note's first words). Nothing
  else carries it: events, logs, notifications, the web event stream, ESP-NOW
  and `note status` hold only ids, phases, counts, timings and reasons.
- Note files obey the transcript rules: open to the account that spoke them
  and to superadmins, nobody else.
- The note events are quiet. `g2_long_press`, `g2_long_release` and
  `note_started` are event-only; `note_saved` and `note_failed` go to the
  notification center queue only. By default none of them shows as a banner,
  a web toast or a card on the glasses.
- The glasses microphone is released when a note ends, whatever ended it.
- The files are plain text. Anyone with the SD card, or a copy of the flash,
  can read them.

## Glasses firmware and the Even app

**Hold profiles.** HardwareOne acts on holds only when both arms run a
glasses firmware it has a hold profile for:

| Glasses firmware (both arms) | Hold source | Evidence |
|---|---|---|
| 2.3.2.14 | reported (ring vs right/left temple) | bench, 2026-10-05 |
| 2.2.9.22 | not reported (every hold is "unknown") | firmware analysis |

With any other version, or mismatched arms, holds are recorded and posted as
events but do nothing. `g2hold status` shows `trusted=no (unprofiled)` and a
hint. `g2holdunprofiled 1` overrides this for testing new firmware.

**Firmware updates.** If you rely on holds, decline glasses firmware update
prompts. A new version stays unprofiled until it has been checked on the bench
and added.

**Even app 2.3.2 Press & hold bindings.** The 2.3.2 app can bind Press & hold
(left, right or ring, with the screen on or off) to its own QuickNote or
another shortcut. That binding is stored on the glasses, so it can stay in
force while HardwareOne is connected. HardwareOne never reads, changes or removes
it, and never answers or imitates the glasses' own QuickNote. If a hold starts
the glasses' own feature, or seems to do nothing, set Press & hold to None in
the Even app. Outside HardwareOne's pages, your binding (or nothing) applies as
usual.

**Switching Even app versions.** Avoid switching between Even app versions
while you rely on 2.3.2 bindings: an older app (2.3.1) that connects may reset
bindings it does not know.

**Going back to the phone.** There is nothing to undo. This feature writes no
persistent setting to the glasses: not the gesture table, the menu, the
dashboard or the ring settings. The Even app finds your setup as it left it.

## P4X-EYE and CM5 builds

| | P4X-EYE | CM5 builds (XIAO-S3 + Raspberry Pi) |
|---|---|---|
| Transcription | On the device (local STT) | On the Pi, which must be signed in and advertise dictation |
| `notemaxsec` | Default 60, up to 120 | Default 30, capped at 30 (the Pi recorder's limit) |
| Pauses | A note runs until it is stopped or reaches `notemaxsec` | The recorder also ends a note after a pause: 1.2 s for a tapped or CLI note, 6 s for a hold |
| Waiting for the text after stop | Up to 30 s | Up to 95 s |
| `note_started` detail | `local` | `pi` |
| Speech model | Stays loaded while the Notes page is open and for 2 minutes after it closes or after the last note, so a hold does not wait ~4 s for the model to load. Transcription and Conversate use the loaded model too. It is freed after that, or at once if free PSRAM drops below 4 MB. Opening the page does not load it while ESP-SR runs, or when free PSRAM could not hold it with 4 MB to spare | Not applicable |

While the model is loaded it holds 20 MB or more of PSRAM. A feature that
needs more PSRAM than is free then (the camera, ESP-SR) fails to start rather
than freeing the model. Leave the Notes page and wait two minutes, then start
it again.

On the XIAO-S3 QuickNote costs flash on a nearly full app partition; a
deployment that needs the room can build with `ENABLE_QUICKNOTE 0`. Holds are
then still decoded, recorded and posted, and `g2holdaction` defaults to Off.

## Checking it

```
g2info                    - firmware version on each arm (an admin can run g2glasses refresh if it shows ?)
g2hold status             - profile, settings, the open hold and the last 16 hold records
g2hold clear              - empty the record ring and counters
note status               - whether a note can start now, and why not
events                    - g2_long_press / g2_long_release / note_* events (no text)
debugg2events 1 temp      - log each hold as it arrives (with loglevel debug)
debugg2 1 temp            - also log glasses messages HardwareOne does not handle, with up to 64 bytes in hex
g2protostats              - message counts per service; 0x12 is the glasses' own QuickNote ("QuickNote?")
```

`g2hold status` prints one row per hold copy or implicit end, for example
`Press→quicknote`, `Press→page:24` (the Notes page), `Press→ignored:source`,
`Press→ignored:refused` or `Aborted(overlay)`; the matching `g2_long_press`
event carries the refusal's reason. Admins can drive the whole path without
the glasses with `g2hold sim press [ring|right|left|unknown]`,
`g2hold sim release` and `g2hold sim abort`; `sim press` also needs the paired
wearer, because it can start a recording.
