# Live transcription test status — 2026-10-11

Working branch: `stt-speedups`. The selected test profile is
`configurations/p4-glasses-test.json` (P4X-EYE, ESP-SR off, SDMMC on, original 15x5 gen2 model selected).

## Behavior

The G2 Transcription app's Live text view now keeps its page and eight body
lines alive, patching only changed text bands. It no longer runs SHUTDOWN/CREATE
for each draft. The latest view shows the most recent eight lines; scrolling
back stays on the older page. Mirrored ring gestures are debounced.

The first deployment closed the view to black on entry (wearer report).
It used nine text children with capture on the header. The initial correction
reduced this to four display-only children, which restored visible incremental
updates but stopped scroll input. That correction interpreted an older canary
too broadly: Q enabled capture on BOTH of its text children, so its rejection
never established that one capturing child is unsupported.

The current correction keeps the four-child shape and enables input capture
only on the single-line header, as required by the SDK's exactly-one-owner
rule. Three body bands retain the same 3/3/2 transcript rows and receive only
in-place text updates. Nonempty placeholders are visible at CREATE; bounded
UTF-8 updates follow after acknowledgment. CREATE is at most 142 bytes and
UPDATE is at most 214 bytes, below the observed 241-byte ATT write limit.
Tests decode the actual CREATE capture flags as 1,0,0,0 and reject zero owners.
Physical scrolling qualification is tracked in the current Food investigation
status; successful builds and boot alone do not establish gesture recovery.


Final phrases retain their sequence-to-speaker association after text ACKs.
Speaker results can update `Person 1`, `Person 2`, etc. while the session
continues, independently of saving. `Person ?` means not assigned yet or
unknown; provisional text never guesses the previous speaker. Speaker labels
require `/sd/STT Models/speaker.spk`. Native Conversate's three-line view is
unchanged. Missing models leave live transcription usable without labels.

## Verification

Sanitizer-enabled host tests passed for the live page, text-page readiness,
continuous STT with speaker labels, feature-off STT, local/CM5/disabled
Dictation, transcription command adapter, and existing Conversate/lifecycle
paths. Enabled/disabled header contracts and actual P4 SD configuration tests
passed. Both selected and baseline P4 firmware builds passed. The corrected
layout also passed the actual-page tests and production-encoder wire-size/CRC
checks under ASan/UBSan.

Blackout-fix application (superseded by the accuracy correction below):
5,454,688 bytes; SHA-256
`6c56697397f5c190f81d78549e4cae3b66d913464ea156d9eee1ea3078883cfc`.
Expected ELF SHA-256:
`03edd4575be2826f89aa779c73066387e9543038c05a81d945241df9cbc6b229`.

Flashed successfully with esptool hash verification, application only at
`0x10000`. This device retains its older `0x615000` application slot:
922,784 bytes (901 KiB) remain. The generated build partition table describes
a different layout, so it was not flashed. Existing filesystem, settings,
models, and bootloader were preserved. The prior application and complete
pre-task flash backup are retained outside Git in the workstation's private
HardwareOne backup directory.

Post-fix hardware retest: boot ELF prefix `03edd4575` matched. SDMMC mounted,
both temples connected, and the native glasses returned CreateSuccess for the
four-child page (131-byte protobuf). Subsequent text updates patched bands
without page swaps; the wearer confirmed "Page stays visible". Logs showed
successful loading of the 1,994,544-byte speaker model and four completed
speaker labels during the wearer's session (about 0.9–2.0 s result lag).
The earlier unauthenticated serial file check is no longer the only evidence:
actual model loading has now been observed. Two-speaker accuracy was not tested.

For further reading-comfort and two-speaker accuracy tests:
open Transcription, start, and choose Live text. Check incremental words,
labels after phrases, scroll/back, and reopening after lens sleep.

## Accuracy correction — original model restored

After the blackout fix, the wearer reported recognition substitutions, including
“camera battery screen” becoming “tumor banervy screen” and “fan charger potato”
becoming “and charger potato”. The available earlier retest logs showed no lost
G2 frames or capture overruns; these later example phrases were not captured.

The original pre-task app backup selected the SD file
`/sd/STT Models/quartznet15x5-gen2-25397.p4.stt`. Its pinned raw model SHA-256 is
`47e58faa01470d32dad88f1faa46e3f61dff0fd605aa542103aefd11502dae72`.
The first builds in this task accidentally used the branch's default 5x5 model
(7,171,984 raw bytes / 71 nodes), despite the original app using this 15x5 model
(20,112,128 raw bytes / 201 nodes). Disabling ESP-SR did not shrink the weights.

The saved profile now explicitly enables `ENABLE_STT_MODEL_15X5_GEN2`.
A shared selection header pairs the exact generated identity and SD path;
ordinary builds retain 5x5. The generated identity is byte-for-byte the archived
export's header, and its raw hash matches both the export and original app.
Configuration rejects this selection without local STT, P4, and SD support.
Model and LM startup diagnostics now expose the selected path/size. No acoustic
weights, LM files, decoder parameters, endpointing settings or text limits were
changed. Existing Live text and speaker-label improvements remain included.

Validation: nine actual-profile/model-container cases passed under ASan/UBSan,
including rejection of wrong model/hash and missing prerequisites. Existing LM
integration and runtime cache variants passed under ASan/UBSan. Saved-config
workflow checks passed (17 tests), both P4 builds passed, and diff whitespace
checks passed. The compiled selected app contains the 15x5 hash/path and no 5x5
hash/path; the baseline app still contains its original 5x5 identity/path.

Current application: 5,454,544 bytes; actual `0x615000` slot
headroom: 922,928 bytes (about 901 KiB).
Application SHA-256: `1939e9be4806bc9690059e8e1c79508f925531133946258f367540d579af1ae6`.
ELF SHA-256: `8221afcaebae0475f27f06571d5dc709f856b3d40f4d5e120222ca219ff676f7`.
App-only flash at `0x10000` succeeded with esptool hash verification. Existing
partitions, filesystem, models, settings and bootloader were preserved.
Post-flash boot matched ELF prefix `8221afcae`; SDMMC mounted and both G2
temples reached Ready. The wearer has been asked to repeat the two phrases.
No new transcription session has started in the captured log yet, so actual
15x5/LM/speaker loading and the wearer accuracy comparison remain unverified.
The bounded five-minute serial capture at `/private/tmp/hw1-model-retest.log`
closed before the wearer's next test; it captured boot and connections, no STT
session. Opening this device's serial port resets the P4, so it was not reopened
just to answer the subsequent startup question. The pre-task backups and
new firmware remain in the private HardwareOne backup directory.

## Wearer feedback and startup/display timing

The wearer reports that recognition appears better with the restored model,
but startup felt slow. No timing capture covered that test, so the exact delay
and 15x5/LM/speaker runtime memory figures remain unmeasured.

Read-only source audit: cold continuous sessions load/verify acoustic weights,
prepare the runtime and LM, and initialize speaker support before processing
drafts; audio capture starts before this work. The first draft needs at least
one second of phrase audio plus computation. Models stay resident during the
session. Between-session preload/retention is currently requested only by
QuickNote/Notes, not native Conversate or Transcription, so reopening can pay
the startup cost again.

Native Conversate already receives early drafts and finals directly from the
shared STT broker, with immediate publication wake and a 200 ms fallback. It
sends changing drafts as nonfinal captions (newest ~150 characters), then full
final phrases. Live text currently alternates status/text queries on 500 ms
poll slots, normally about one second between draft fetches, plus scheduling.
More visible lines do not make STT compute earlier. No code, configuration or
firmware changes were made for this timing explanation.


## 800 ms Live text polling and recording-based plan — 2026-10-11

The wearer reports better recognition with the restored 15x5 model and requested
an approximately 800 ms Live text refresh. Visible, active Live text now fetches
text every 800 ms and status separately every 1000 ms. Explicit actions and ACKs
retain priority; one outstanding command is allowed; overdue polls are ordered
by their deadlines. Home/off-lens/inactive/recovery behavior is unchanged.
Unchanged text still causes no lens writes. Command/inference/BLE delays can
exceed the target, which does not change recognition computation speed.

Actual-page ASan/UBSan tests passed: 800/1000 ms cadence, no unchanged writes,
one-band draft patch, slow replies, ACK priority, eventual status, rollover,
and existing ownership/navigation/recovery/save/receipt cases. Independent
source review found no blocker. Both selected-profile and P4X-EYE baseline
builds passed; diff whitespace checks passed. The baseline retains its default
5x5/ESP-SR/no-SD selection and was not flashed.

Selected image: 5,454,656 bytes; existing device slot 0x615000; free 922,816 bytes
(about 901 KiB). App SHA-256:
`265054a3c76f958690b9c398af72a59aeca3433a4e0bc93678cd8faa9f6823b8`.
ELF SHA-256:
`5e4060aac64be0637bb8e7d8aee199a7ca0438b7afb3c406d56bc24b47a8f3cf`.
The app-only flash at 0x10000 succeeded with esptool hash verification.
The old partition table, bootloader, filesystem, settings and SD models were
preserved. Subsequent capture observed setup completion and both G2 temples
Ready. Capture began after early ROM/app metadata and SD mount messages, so it
does not independently attest those messages. Wearer verification of the new
800 ms behavior remains pending. Firmware and logs are in the private backup.

Authenticated reads retrieved the last listed transcript and its preceding
same-boot session before flashing. The latest contains 13 Person 1 phrases
about excess line breaks, repeated tags, punctuation and screen space. The
preceding recording is the speed/word test. The clock was unsynchronized, so
selection relies on directory/sequence order and matching content, not a
reliable timestamp. Private copies live outside Git. The saved credential was
rejected by the device; the user's subsequent local interactive sign-in worked.
No credentials were copied to project files or the plan.

A serial subclass that suppresses DTR/RTS updates and clears HUPCL allowed these
reads without resets, correcting the earlier plain-pyserial limitation. The
flash itself intentionally reset the device.

See [STT_LIVE_TEXT_NEXT_STEPS.md](STT_LIVE_TEXT_NEXT_STEPS.md) for the proposed
speaker grouping, provisional text, reflow and punctuation milestones. No
speaker-paragraph grouping or punctuation change is included in this image.


## Grouped speaker paragraphs — 2026-10-11

Continued the recording-based plan after the user's request to resume. Live
text now joins adjacent finalized phrases with the same known speaker into
one paragraph, showing the speaker tag once per turn. Unknown and evicted-label
text uses a neutral `?:` block; the live draft uses a `~` prefix. Without speaker
support, finalized text flows as plain prose. Original text and per-sequence
phrase records are retained for delayed labels, receipt replay and save behavior.
Recognition, model/config choices, punctuation and saved-file formatting did
not change. The 800 ms text / 1000 ms status polling remains in effect.

Bounded source spans and a source-byte reading anchor keep committed text in
view when late labels reflow the lines. An explicit follow-tail flag follows
new words when desired. Navigation remains reachable between page boundaries,
and a navigation generation check prevents an old render from overwriting a
newer scroll. Drafts lack a phrase ID in the current response; when finalized
or cleared, a draft reading position falls back to a committed boundary, not
a guessed offset in a later utterance. If no committed source remains, the
fallback is the oldest current line.

Validation passed: expanded complete-page harness with ASan/UBSan, production
wire encoder with ASan/UBSan, six disabled-G2-header tests, independent source
review, both firmware builds and diff whitespace checks. Fixtures cover 13
same-speaker phrases; A/unknown/A joining/splitting; provisional replacement;
split-word receipt continuation and duplicate replay; speaker-disabled prose;
16-record metadata eviction; clipped UTF-8 tail; metadata-only follow-tail and
older-text reflow; neighboring line reachability; navigation during rendering;
and final/new-draft/empty-history fallback. Tests use synthetic text, not private
recording content. CREATE <=142 B and UPDATE <=214 B fit the observed 241 B ATT
payload. The first device build caught a non-trivial-object memset warning
promoted to an error; value-resetting those records fixed it before deployment.

Selected image: 5,455,840 bytes (+1,184 from the previous image), with 921,632
bytes free in the actual 0x615000 application slot (about 900 KiB).
App SHA-256: `33ea598dea79afcbe74ac60f441c282b362e47ae852b5edf9431b6cca6405e34`.
ELF SHA-256: `c2494d0151b1159f832e0df8c1f5172491daa4940cb833d0fb846c9570173190`.
The selected binary contains the pinned 15x5 identity/path and no default 5x5
path. Baseline build remains 5x5, ESP-SR on, SD off, and was not flashed.

App-only flash at 0x10000 passed esptool hash verification. Immediate no-reset
capture matched boot ELF prefix `c2494d015`, observed LittleFS and SDMMC mounts,
setup completion and both G2 temples Ready. Existing partition table,
bootloader, filesystem and SD models were preserved. The previous
`live-800ms-15x5.bin` and original backups remain available; the new image and
verification logs use the `live-speaker-paragraphs-` prefix in the private
backup directory. The wearer has been asked to test pauses, grouping and
scrolling. On-glasses comfort and two-person accuracy are not established by
build/host tests; wearer feedback is still pending at this checkpoint.

Plan milestones 2–3 are implemented and deployed for wearer testing. Automatic
punctuation and formatted saved-session reading remain subsequent work.


## Wear-detection command attempt — 2026-10-11

User requested `g2glasses weardetect off`. Serial sign-in succeeded through the
local hidden-password prompt. The initial setter reported right-arm unavailable:
the BLE links remained up, but the right app interface had been marked plugin
silent while idle. A cache-preserving `closeg2` / `openg2 saved` reconnect brought
both temples to Ready without rebooting the P4 or changing firmware.

The subsequent setter definitively refused the right temple firmware
`2.3.2.14` as an unsupported settings profile. The current allowlist only admits
`2.2.4.34` and `2.2.6.10`. A read-only audit of this checkout and main found no
qualified 2.3.2.14 wear-setting implementation or capture; existing 2.3.2.14
qualification concerns hold gestures only. Settings refresh returned no snapshot.
No wear-setting write was sent after the version guard; wear detection was not
changed. No firmware or source code was altered for this request. Further work
requires verified settings-protocol support for 2.3.2.14, not a blind allowlist
extension. Private attempt logs: /private/tmp/hw1-wear-reconnect-1791747865238529000/.


## Wear-detection patch verified — 2026-10-11

User authorized the follow-up patch. Native settings now use promoted RIGHT
readiness, changing request IDs and request/connection-correlated confirmation.
Wear writes alone admit exact paired firmware 2.3.2.14 after a full BASIC reply.
OFF scalar omission is qualified by the full reply; partial updates remain unknown.
Details and primary evidence: [G2_WEAR_DETECTION_23214.md](G2_WEAR_DETECTION_23214.md).

Both firmware builds and focused production-path host tests passed. Selected
15x5/SDMMC/ESP-SR-off app: 5,458,176 bytes, leaving 919,296 bytes in the actual
slot. App SHA-256: `e6df3d1245a26e06ce2e2066e933acfda1ab37d28c151c9ece49a76926fc6bcf`.
App-only flash passed hash verification; boot ELF prefix `1fa2847c1` matched,
filesystems mounted and both G2 temples reached Ready. Previous app retained.

After local sign-in, two reads reported wear ON; OFF write magic4 received the
RIGHT matching ACK `08 01 10 04 1a 02 2a 00`. Fresh full replies magic5 and6
confirmed OFF (default-elided f10), and explicit refresh printed `weardetect off`.
This succeeded after the renderer had been marked plugin-silent, qualifying the
readiness fix as well. Wear detection is now OFF. No other setting was written;
protocol debug verbosity was returned to OFF. Reverse ON and glasses power-cycle
persistence were not tested. Private capture and durable backup paths are in the
patch note.


## Scroll input follow-up — 11 October 2026

The four-child all-text correction initially disabled capture everywhere, so the wearer could see updates but scrolling emitted no events. The earlier claim that compound text capture is prohibited was too broad: Q enabled capture on both of two children, and the first live failure used nine children. Official SDK rules require exactly one capture owner. The follow-up enables capture only on the existing header and retains the layout and UPDATE_TEXT path. Tests now enforce exactly one owner instead of encoding the blanket ban. See `STT_FOOD_AUDIO_INVESTIGATION_20261011.md` for current build and hardware qualification status.


Wearer confirmation, 11 October 2026 (ELF `d4e15f5bc`): “This works well. The diarization works great.” The scroll correction is now confirmed by both the hardware event trace and wearer feedback. Diarization feedback is qualitative for this replay; no new accuracy score was measured.
