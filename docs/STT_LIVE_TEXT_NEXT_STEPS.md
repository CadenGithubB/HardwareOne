# Live text: next steps from the glasses recording

Prepared 2026-10-11. The 800 ms polling update and the first speaker-layout milestone are implemented. Automatic punctuation and formatted saved-session reading remain planned. See STT_LIVE_TEXT_TEST_STATUS.md for build, flash and wearer-validation status.

The latest listed recording says Live text is faster, but starting a new line at every pause wastes the display. Continued speech from the same person should flow along the current paragraph, with useful punctuation. The recording contains 13 finalized chunks, all tagged Person 1. The preceding recording is the Conversate speed and word-recognition test.

Source: authenticated device read of `/sd/stt/u1/boot-local-c771163a00000002.txt` (744 bytes, SHA-256 `856dfb3ce912eb03e5a16e2355cc234874c777c467645ab9870461379d6bad5a`). The device clock was unsynchronized (`Started: boot`), so there is no reliable wall-clock recording date. Selection used the final directory entry, its session sequence, and matching content. Private originals are retained with the workstation's device backup, outside Git.

## 1. Completed: fetch Live text every 800 ms

Visible, active Live text now polls for text every 800 ms, independently of one-second status checks. Unchanged text causes no lens write; changed text patches the existing bands. Explicit actions and receipt acknowledgements keep priority, with at most one outstanding request. Slow STT computation, command work or BLE delivery can still delay visible words; this is a fetch target, not a guarantee of fresh words every 800 ms.

The actual-page ASan/UBSan tests and both P4 firmware builds passed. The selected glasses image was flashed with hash verification, and both temples reached Ready. Wearer confirmation of this specific interval remains pending.

## 2. Implemented: paragraphs grouped by speaker

`buildLabelledText` in `components/hardwareone/G2_Page_Transcription.cpp` now joins adjacent finalized phrases with the same known speaker ID using a space. `Person 1:` appears once at the start of that run; a different known speaker starts a new paragraph/tag. Text wraps across the available eight lines. When speaker detection is unavailable, finalized text flows as plain prose. Unresolved content uses a compact `?:` block and never silently inherits a known speaker.

Raw text, sequence numbers and per-phrase spans are retained, so delayed speaker results can still join or split displayed paragraphs. Receipt acknowledgement, recognition segmentation and saved raw text remain independent of presentation. Speaker IDs are session-local voice clusters, not identified people.

Passing production-page checks:

- Thirteen consecutive Person 1 phrases produce one wrapping paragraph and one initial tag, with every recognized word retained in order.
- Person 1 → Person 2 → Person 1 produces three distinct turns, even when labels arrive late.
- Unknown labels never silently inherit a known speaker's identity.
- Duplicate receipts, UTF-8 text, a trimmed history and the 16-phrase metadata limit preserve text and valid labels.

## 3. Implemented: provisional text and stable reading position

Incoming draft words appear with a small `~` prefix on a provisional line; no speaker identity is guessed. Finalized phrases join the preceding turn once a matching label arrives. A different result starts a new turn. Words are not held back while speaker analysis runs.

Only changed display bands are updated on the existing four-child page. A bounded source-byte anchor preserves the reader's position when labels change wrapping; explicit follow-tail state keeps the newest text visible when appropriate. Adjacent navigation remains reachable even when reflow places the anchor between page boundaries. Drafts have no phrase identity in the current protocol: when a final arrives or a draft clears, an older draft reading position falls back to a committed boundary rather than attaching to a later utterance. If its words have left the retained tail, the view falls back to the oldest retained content.

Passing ASan/UBSan checks cover delayed labels, unknown-to-known transitions, run splitting, unchanged polls, receipt replay and continuation, UTF-8/history eviction, scroll/reflow, draft replacement, navigation during rendering, lens sleep/reopen, ownership changes, and stable page creation. Wire tests confirm CREATE <=142 bytes and UPDATE <=214 bytes, below the observed 241-byte ATT limit. Wearer validation should include a real two-person conversation and one speaker pausing frequently; build/host checks do not establish on-glasses reading comfort. Current metadata reports completion, but not tentative-assignment confidence; evaluate exposing that existing confidence separately if short-phrase mislabelling is observed.

## 4. Punctuation as a separate quality step

The current decoders emit lowercase letters, spaces and apostrophes. There is no existing punctuation-restoration output for the page to display. A pause is not reliably a sentence ending, so do not add a period after every STT chunk.

First deliver the space-saving grouping without altering recognized words. Then evaluate a bounded punctuation/capitalization pass on finalized text, outside the latency-critical draft path. Preserve supplied punctuation and the raw transcript. Any simple display capitalization must be documented as formatting, not recognition. Commas, question marks and sentence boundaries need their own quality check; do not promise them from pause timing alone or add a new model without measuring memory and latency.

Use representative recordings with mid-sentence pauses, questions, names and interrupted speech. Reject a punctuation approach that changes meaning, drops words, destabilizes the live display or noticeably delays final text.

## 5. Reuse proven formatting in saved-session views

After Live text is comfortable on the glasses, consider a formatted saved-session reader that groups consecutive speaker lines while preserving the original transcript and its finalization marker. Reuse the same formatting policy if both readers need it, rather than creating divergent rules. Account for the existing 512-byte file windows and labels split across windows. Native Conversate remains a separate, three-line consumer; assess its presentation separately after the larger Live text view is validated.

Each firmware milestone should remain independently reviewable, pass focused production-path tests plus the selected profile and P4 baseline builds, and be tried on the glasses before expanding scope. Keep the restored 15x5 model and disabled ESP-SR selection throughout these UI milestones. Flash only the application into this device's existing `0x615000` slot.
