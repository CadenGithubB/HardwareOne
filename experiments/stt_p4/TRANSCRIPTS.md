# Saving recognized text

Transcript saving is an optional, shared feature for local P4 STT and the
existing Pi-backed Dictation provider. P4 microphone saving and reboot retention
are qualified in [TRANSCRIPT_RESULTS.md](TRANSCRIPT_RESULTS.md). Pi transcript
saving has host coverage but has not been tested physically. Earlier STT
qualification reports predate this feature.

## Enable it for the next session

The persistent **Save STT transcripts** setting defaults to **off**. It uses the
normal settings registry, so the generic web, OLED and G2 settings editors expose
it where Dictation or local STT is enabled. There is no separate transcript page.
An authenticated administrator can also use:

```text
sttsavetranscripts
sttsavetranscripts 1
sttsavetranscripts 0
```

The first command reads the setting; the others enable or disable it. The saved
JSON key is `stt.sttsavetranscripts`. This is a device setting, but each admitted
session captures its own on/off value and initiating account. A change during a
session applies to the next session. Turning it off does not delete older files.

For a short local test, enable saving before starting:

```text
stt record 8
stt status <id>
stt result <id>
```

Replace `<id>` with the returned 16-character run ID. Use the same authenticated
session for status and results. Wait until recording is ready before speaking,
and until the worker has finished before checking final save status. Continuous
use still follows the existing `stt start`, `stt next`, `stt ack`, `stt stop` and
`stt cancel` protocol described in [README.md](README.md). Saving does not drain
the result mailbox: continue reading and acknowledging chunks to avoid overflow.
The OLED/G2 Dictation path captures the same option automatically. Local Dictation
passes it to STT; it does not save a second copy. Pi Dictation saves its accepted
final result through a background worker, independently of UI delivery.

## Files and status

Each session with nonempty recognized text creates one plain-text file in the
initiating account's numeric directory:

```text
/stt/u<accountId>/<stamp>-local-<runId>.txt
/sd/stt/u<accountId>/<stamp>-local-<runId>.txt
```

Pi results use `-pi-` instead of `-local-`. Account IDs are persistent numeric
IDs, not usernames. Use the actual returned path rather than guessing an ID.
`<stamp>` is the admission time in UTC, such as `20260928T235959Z`, or `boot` when
the clock is unsynchronized. The 16-hex-digit run/exchange ID distinguishes files.
Existing files are never overwritten on a filename collision.

A header records the start stamp and backend. Each accepted nonempty result is
appended as a line; the writer does not add punctuation or improve recognition.
Local chunks are limited to 512 text bytes and a Pi final result to 256 bytes.
There is no accumulated whole-session transcript in RAM. This option saves text,
not microphone audio, and does not change the existing Pi audio transport.
Silence-only sessions create no file.

Authenticated `stt status <id>` includes:

| Field | Meaning |
| --- | --- |
| `transcriptEnabled` | Option captured when this session was admitted. |
| `transcriptSaved` | A file has been opened for writing; inspect the error and completion fields before treating it as complete. |
| `transcriptComplete` | The final end marker was written and the writer's checks passed. |
| `transcriptChunks` | Successfully appended nonempty text chunks. |
| `transcriptBytes` | Bytes written, including the header, separators and end marker. |
| `transcriptPath` | Session file path, available after storage selection. |
| `transcriptError` | Saving failure, separate from the recognition error. |

Final markers are `[End: done]`, `[End: cancelled]` or `[End: failed]`.
`transcriptComplete` means file finalization, not successful recognition of the
whole session. A cancelled or failed session can have a finalized saved prefix.
A missing end marker means finalization was not confirmed. Shared Dictation
status reports saving state/error but omits the private path from its general
snapshot.

## Storage, stop and failure behavior

Storage is selected at the first nonempty write: writable SD first, otherwise
internal LittleFS. That tier remains fixed for the file. There is no mid-session
fallback if an SD card disappears or becomes full, no rotation, and no automatic
old-file deletion. Each write requires roughly 100 KiB of remaining space plus
its payload and bookkeeping allowance. A selected SD card with insufficient
space produces a saving error rather than moving the file to internal flash.

A worker writes and closes the file after each append; capture, UART receipt and
UI callbacks do not perform transcript filesystem writes. Normal stop drains
admitted audio and saves resulting accepted text. Cancellation retains text
already accepted for saving, but does not promise to transcribe queued audio or
an unfinished inference. A Pi result already accepted into its save job survives
UI cancellation or result consumption. Repeated delivery/acknowledgement does not
append the same result twice.

The original authenticated session must still be live when the first write is
made. Later appends retain that account binding; logging in as someone else or
recreating a deleted username cannot claim the file. An unresolved/revoked first
owner fails saving without stopping transcription. Once saving encounters an
error, it stops appending for that session, leaves any partial file in place,
and reports the error while normal text delivery continues. Editing, moving or
deleting a file while its session is active can therefore stop saving.

Appends call flush, check write counts and file size, and close the file. The
underlying Arduino filesystem does not expose all flush/fsync failures here.
These checks do **not** guarantee recovery of the latest text after sudden power
loss or card removal. The end marker is an application finalization indicator,
not a power-loss durability guarantee. Internal-flash writes also consume flash
write endurance; manage retained files through the existing file tools.

## Retrieve a transcript

The existing web **File Manager** at `/files` can browse the appropriate `stt`
account folder and **Download** the `.txt` file. Download uses the authenticated,
permission-guarded `/api/files/read?name=<URL-encoded-path>` endpoint. It does not
need a new page or a separate cloud service. Web retrieval requires the normal
working HTTP connection; saving local P4 transcripts itself does not need Wi-Fi.

The existing file CLI commands require an authenticated administrator and still
apply account-path permissions. For example, list the internal or SD root:

```text
files json "/stt"
files json "/sd/stt"
```

Only use the tier that exists on the device. Ordinary accounts see their own
allowed subtree. For a complete CLI download, replace the example path below
with the real `transcriptPath` and request bounded chunks:

```text
fileread "/stt/u2/20260928T235959Z-local-0123456789abcdef.txt" 0 512 b64
```

Decode each successful reply's base64 `data`, then advance the offset by the
returned `len`, not the requested size, until `eof` is true. Stop on an error or
a zero-length non-final reply. Download after finalization for a stable file.
Paths must be double-quoted. Transcript `fileview` is intentionally refused:
use `fileread` or the web download so text does not enter the general paged
console broadcast path.

## Privacy and limits

The file guard isolates ordinary users and administrators by account ID;
superadministrators and scoped trusted system operations retain their existing
privileged access. Anonymous and synthetic authentication-bypass identities are
not transcript owners. Protected CLI file reads and STT status/results use the
private reply path; shared command mirrors redact their protected contents.
Saving is separate from acknowledgement and remains opt-in for future sessions.

Files are plain text, **not encrypted at rest**. Someone with physical access to
the card or flash can read them. Downloaded copies are also ordinary text files.
This feature does not add retention expiry, a transcript search index, audio
recording, streaming token output or any change to model accuracy and latency.
