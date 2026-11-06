# Speech vocabulary, display formatting and transcript metadata

Integrated 21 October 2026 from the standalone `HardwareOne_Speech_To_Text`
handoff (its qualified standalone app ran on a P4X-EYE with the same pinned
15x5 gen2 model). This records what was ported into the firmware, how it is
wired to the existing owners, what was deliberately not ported, and what still
needs hardware qualification. The firmware's speech engine, model, LM file,
speaker labelling, authentication, guarded storage, receipt semantics and
session lifecycle are unchanged.

## What a user gets

- **Readable live text.** Recognized text is shown with whitespace collapsed,
  standalone *I* and its contractions capitalized, configured terms in their
  chosen spelling (`hardwareone` → `HardwareOne`, `esp` → `ESP32`), and the
  first word of a paragraph capitalized. No punctuation is guessed and no word
  is invented. The decoder's raw words are what is acknowledged and saved.
- **An editable vocabulary** of names and terms, without retraining, rebuilding
  the language model or reflashing: one entry gives the decoder a hotword and
  the displays its spelling. `sttvocab list | add DISPLAY[|spoken] | remove
  <spoken> | reload` on any authenticated console; the Sensors page's
  Transcription panel has a Vocabulary section. Edits apply to the next phrase.
- **Grouped browser view.** The web Transcription panel now groups finalized
  phrases by speaker like the lens page (`Person 1:` paragraphs, `?:` for text
  still waiting for a label) and shows the live draft as a `~` line.
- **Ordered saved transcripts offline.** Each transcript's header records the
  boot number, power-on count, this boot's session number, an NVS ordering
  domain and a title taken from the first phrase, so transcripts made with no
  clock (`Started: boot`) still list in order.

## Owners and wiring

| Piece | Owner | Notes |
|---|---|---|
| Vocabulary table and formatter (portable) | `components/hardwareone/stt/stt_transcript.{h,cpp}` (`hw1::transcript`) | Ported from the handoff's `hw1_transcript`; no Arduino/IDF/VFS/heap. `nextBoot` was not ported: `System_BootState` already owns the counters. |
| Vocabulary file, locking, hotword snapshot, display copy | `System_STTVocabulary.{h,cpp}` | Loaded once at boot after the filesystem (`sttVocabularyInit`) and on `sttvocab reload`. Device-wide; edits need an administrator. |
| Hotwords into the LM decoder | `System_STTLocal.cpp` `loadCustomWords` | The vocabulary's spoken forms are the `CustomWords` list. Re-taken between phrases when the vocabulary generation moves; a decode in progress is never changed. **The fixed `custom_words.txt` is no longer read**; its presence beside the LM is logged once with that advice. |
| Display copy for the UIs | `System_TranscriptionUI.cpp` (`transcription next`) and `WebPage_Sensors.cpp` (`GET /api/transcription`) | Reply gains `display` (and a formatted `draft`); `sttText` and the receipt keep the raw bytes. The first phrase of a session starts its paragraph. |
| Lens live page | `G2_Page_Transcription.cpp` | Shows `display`; a known speaker's turn (or the prose when labels are off) starts with a capital. `?:` text keeps the decoder's casing so it does not change case when a label joins it to a turn. Tail bytes and scroll anchors are the display text consistently; receipts stay raw. |
| OLED live mode, web live view | `OLED_Mode_Transcription.cpp`, `System_Transcription_Web.h` | Show `display`, fall back to `sttText`. The web view keeps phrases per sequence and regroups on late labels. |
| Transcript header | `System_Transcript.cpp` | `Boot:`, `Power-on:`, `Session:`, `Domain:`, `Title:` lines. The saved lines stay raw. |
| Title rule shared with notes | `Transcript_TextPolicy.h` | `QuickNote_Policy.h` forwards to it, so a note and a transcript of the same speech get the same title. |
| Boot ordering | `System_BootState` | `poweron` advances on `ESP_RST_POWERON`; `domain` is drawn once per NVS generation. Both read back for the header. |

## Vocabulary file

`/sd/STT Models/user_vocab.tsv` (the card copy wins, like the model and LM),
else `/STT Models/user_vocab.tsv`. One entry per line: `DISPLAY`, or
`DISPLAY<TAB>spoken` when the spoken form is not just the lowercase display:

```text
HardwareOne
OpenAI
ESP32	esp
```

Limits: 256 entries; spoken form 1–31 lowercase `a-z` with internal
apostrophes (the decoder's alphabet); display 1–63 printable ASCII bytes with
no whitespace or `|`. A term with digits needs an explicit spoken form.
Multi-word mappings and pronunciation learning are not implemented. An invalid
file or edit is rejected as a whole and the active list is unchanged. Edits are
written through the shared staged writer (`VFS::StagedWrite`): the previous file
survives until the new one is complete and checked; on SD it is kept as `.bak`.
This is not a power-loss guarantee.

Hotwords bias the existing decoder (bonus from the LM header); they do not
teach pronunciations or guarantee recognition, and they apply only when the LM
loads. Display spellings work with greedy decoding too. Judge a new term
against real audio, not by a successful edit.

## Transcript header

```text
HardwareOne transcript
Started: 20261021T153012Z        (UTC, or "boot" while the clock is unsynced)
Backend: local
Boot: 77
Power-on: 4
Session: 3                       (this boot's run number; low half of the id in the file name)
Domain: 3f9a12c4                 (NVS ordering domain; counters compare only within one)
Title: Buy oat milk and call the dentist
An End marker below confirms finalization.

<raw lines, "[Person N] " prefixed when speaker labels are on>

[End: done]
```

The power-on counter counts `ESP_RST_POWERON` resets, not a perfect
plug/unplug history. An NVS erase (which the usual erase-before-flash does)
restarts both counters and draws a new domain, so two transcripts agree on
chronology only when their `Domain:` matches. Previously unknown dates are never
rewritten.

## Not ported, and why

- The standalone JSONL recording format, its USB `STT_EVENT` stream, its serial
  command parser and its loopback Python bridge: the firmware already has an
  authenticated transcript writer, console and web/BLE/lens transports.
- The standalone's `note start/stop` boundary semantics: the firmware keeps its
  own stop/drain/cancel.
- The browser viewer's import/search/export of whole recordings and its
  in-memory title/alias annotations: the saved-transcript reader is unchanged
  (512-byte raw windows). Formatting of saved files on read, titles in file
  lists and saved alias edits remain open.
- The handoff's `custom_words.txt` merge: replaced outright (one list, one
  owner).

## Verification

Host suites (all green, ASan/UBSan where the suite supports it):
`stt_transcript_tests` (new), `stt_vocabulary` (new), `stt_local_lm` (hotwords
from the vocabulary, re-merge between phrases, legacy file notice),
`test_transcript.py` (header lines and title), `quicknote_policy_tests` (title
rule moved), `transcription_ui_adapter`, `oled_transcription`,
`g2_transcription` (display copy, turn capitalization), `transcription_web`
(handlers plus the browser script: display, turns, drafts, vocabulary editor;
run with a Deno-backed node shim on this Mac).

Firmware builds (2026-10-21, on the current uncommitted tree):

| Profile | Result |
|---|---|
| `p4-glasses-test` (local STT, LM, speaker labels) | green; image 5,572,832 B, 1,853,216 B (25%) headroom; +16.9 KB over the morning's build of the same tree |
| `xiao-no-bluetooth` (CM5 dictation, web sensors, no local STT) | green; image 4,406,816 B, 1,577,440 B (26%) headroom; +6.5 KB (web turn grouping, handler metadata, header, counters) |
| `xiao-default` | **does not fit**: 6,015,376 B against a 5,984,256 B partition. Its last green build (2026-10-18) had 5,952 B of headroom and the tree has since gained the uncommitted Bluetooth lifecycle rework; this change adds roughly 6 KB there, so the overflow is mostly not this feature's, but the profile needs a size decision before it ships. |

Flashed 2026-10-21 to the P4X-EYE the handoff qualified (MAC placeholder 02:48:57:31:04:68,
revision v3.2): app slot only (`0x10000`, hash verified), replacing the
standalone captions app that was on it; NVS and LittleFS untouched. The device
reached "Setup complete", associated Wi-Fi and reconnected the left temple and
the ring. The serial console requires a login, so the on-device checks below
were not run from the bench.

**Not yet done:** qualifying on the glasses (vocabulary edits during a live
session, lens/OLED/web display, a transcript saved with no clock and its
header, the `custom_words.txt` notice on a card that still has one).
