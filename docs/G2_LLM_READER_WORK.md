# G2 reply reader and voice follow-up

> Historical development and device-test record transferred on 2026-10-13.
> Build, installation and flash statements below describe the originating
> workstation and its devices; they do not confirm the current checkout has
> been built or flashed. Device and network identifiers are redacted.

Current follow-up (2026-10-13): user requests more text per reader page and native
Hey Even. The combined six-line reader/native/radio image was app-only flashed.
The first native test exposed a renderer-silence check incorrectly rejecting
the independent native service. That fix restored speech and agent replies,
but the lens answer remained invisible, including for ASCII-only text. The
current image includes nonempty reply finalization and a corrected native
packet bound: `5c7e6c96b875a89a5200d0fa45cf157628f99babbed6ae41aefc141006127a41`.
Both lenses and Wi-Fi are connected, HTTP is READY, and the C6 has zero transport
errors or recoveries. Both saved firmware profiles and native host regressions
passed. The wearer confirmed the native short answer now appears; matching
STREAM_COMPLETE events are recorded at 14:34:02 and 14:34:35 UTC in
`work/g2-llm-reader/native-debug-20261013T143210Z.log`. The subsequent six-tip
native reply also displayed in full with working scrolling, confirmed by the
wearer. The wearer then confirmed the taller custom reader, paging, and hold
follow-up work. Final serial evidence is
`work/g2-llm-reader/combined-watch-20261013T143750Z.json`: the six-tip answer
contains 862 bytes, HTTP is READY without error, both lenses are connected,
and C6 transport/recovery counters remain zero. Temporary debug settings were
restored and the serial port released without a device reset.

Final cleanup removed the four task-owned generated directories
`build-config-p4-openclaw`, `build-config-p4-glasses-test`, `managed_components`
and `.sdk-overrides`, reclaiming 2,774,634,496 allocated bytes (2.584 GiB).
Recovery artifacts, source freezes, dependency reconstruction inputs and the
absence of active builds were checked first. All 2,209 canonical Arduino files
verified again afterward. The selected combined image/debug/configuration set,
baseline evidence and prior rollback sets remain under `work/g2-llm-reader/`;
see `combined-cleanup-20261013.json`. No new checkout was created.

2026-10-13: user approved a full-width paged reply reader and a plain hold
shortcut into the existing microphone/review/send flow, keeping the same chat.
Scroll/tap reads pages; double-tap returns to the existing LLM action menu.
Keep the user's reading position while a reply streams. Raise bounded reply
retention consistently rather than merely enlarging the lens rectangle.

The initial reader task owned the Apps/LLM viewer and prompt-entry integration in G2_Glasses,
TextEntry's optional microphone startup, and reply retention consumers. The
separate native Hey Even/STT work in this checkout supplied the native adapter,
HAL_Audio and STT changes. Its source is preserved in the combined candidate.
The current root integration task owns hardware and main's generated builds;
the original side task has finished and no longer owns those directories.

Temporary evidence and source patches live in `work/g2-llm-reader/`; only small
source snapshots are made. Reuse this task's idle `openclaw-p4-stt/hardwareone`
P4 OpenClaw and speech-baseline build directories after preserving the selected
image/ELF/map recovery set. No new project or dependency tree is needed.
Physical hold admission, microphone submission and a normal spoken follow-up
answer are qualified below.

Coordination: exchanged scope and API ownership directly with the native
Hey Even session on 2026-10-13 at the user's request. Both use the shared
`ChatRunToken`/`chatGetRunToken`/`chatReadRun` API, so reused backend session
numbers cannot mix replies. Native source remains separate from this task's
initial reader hardware image; no native code is discarded from main.

Implementation: one input-capturing TEXT widget replaces the left action list
and narrow reply pane. The worker owns pagination and PSRAM buffers; fenced
navigation/hold callbacks only post intent. Pages retain their position as text
grows, clamp at either end, and preserve UTF-8 boundaries. Long replies are
retained up to 16,383 bytes with the HTTP truncation notice preserved.

The taller-page follow-up reuses the existing named TEXT REBUILD path, with
acknowledgement and protocol fragmentation. The original single UPDATE limited
the body to 155 bytes, usually only three full lines, despite the widget already
being 560×272 pixels. Each page can now fill six 48-cell body lines plus the
header/footer (about 224 pixels at the measured 28-pixel line height). The body
allows 582 UTF-8 bytes and the composed frame is bounded at 631 bytes; each
fragment remains within the qualified 241-byte ATT write limit. The widget's
name, geometry and event capture are retained, as are paging and hold controls.
Generation, lifecycle and presentation fences run between fragments, immediately
before each ATT write and at acknowledgement; local display retries never
submit an agent request again. Hardware readability of
the taller layout was confirmed by the wearer in the final combined image.

Taller-page host verification passed under ASan/UBSan: production REBUILD
encoding and fragment sending (including full UTF-8 pages, CRC and fragment
boundaries); production reader worker and named REBUILD with replacement during
encoding, sending and acknowledgement; and the existing native settings suite
with actual per-ATT replacement cancellation. No device settings or hardware
were changed by these host tests.

Plain hold follows the configured admitted input sources and starts the existing
Mic countdown only when generation is idle. It does not send on release: finish
capture through Mic/Keys, review/edit the transcript, then choose Done. Cancel
returns to the reply. TextEntry admission retains the original lifecycle, so a
stale reader cannot open a microphone on a replacement glasses session.

Host verification passed (ASan/UBSan where supported): reader layout with
production wire encoder/parser; production reader runtime; TextEntry startup
and enabled/disabled interfaces; HTTP parser/worker/chat lifecycle/turn serving;
existing hold dispatcher, hold decoder, text navigation, text readiness and
dictation UI suites. Reader review found and fixed stale page publication,
stale microphone admission, return-target sharing and overlay handoff edges.

Build source is frozen under `work/g2-llm-reader/stage/` (13 files), with
`firmware-reader.patch` and `firmware-stage.json` recording its exact composition.
The selected pre-change image/ELF/map and flash/configuration metadata are kept
in `work/g2-llm-reader/previous-qualified/`. The recovery and reader candidate
sets together now use about 65 MB after verified ELF/map compression, reclaiming
122,043,929 bytes; app binaries remain directly flashable.
It is a recovery set, not a duplicate build or dependency tree.


Both isolated reader builds passed: `p4-openclaw` image 5,818,416 bytes with
1,607,632 bytes free in the app partition; disabled-chat `p4-glasses-test`
image 5,469,536 bytes. Bootloader and partition table are unchanged. The reader
candidate SHA-256 is
`8e5f9c16a305d81355887a398411f4f0b3506f803ba0bc0a3a465487011f7ae6`.
Both build trees are temporarily retained in the existing donor for the radio
fix and physical qualification; remove disposable compiler outputs after
preserving the final selected compact recovery set.

Physical qualification encountered a pre-existing radio fault before any reader
page opened. It reproduced after software reboot, a user power cycle, and an
app-only rollback to the saved pre-reader firmware. The full old-image boot log
is `work/g2-llm-reader/previous-qualified-boot-20261013T125748Z.log`.
Wi-Fi SDIO sends fail with ESP_ERR_INVALID_ARG (258) before Bluetooth starts;
queued recovery subsequently leaves BLE teardown incomplete at STOP_CONTROLLER.
Source inspection found a shared TX pool stride of 1600 bytes, incompatible with
128-byte P4 PSRAM/cache DMA alignment. Alternate blocks are misaligned; the
SDMMC driver rejects them before transfer. The bounded alignment fix uses the pinned Hosted SDK override and the actual
IDF allocator hook. Its ASan/UBSan regression compiles production allocator,
pool and SDMMC code: the original rejects 12/25 buffers, while the corrected
stride accepts 25/25, including fallback/cache/encryption cases. The corrected firmware and hardware results are recorded below.
The P4 temporarily ran the saved old image for this comparison. Settings,
Wi-Fi, credentials, CA and relay token are preserved; no erase was performed.

A single houseplant pagination request returned a stream error after 31.4 s.
This is consistent with the relay's previous 30 s upstream idle limit, but the
old relay did not log handled errors, so the exact upstream cause is unproven.
The relay now uses its existing 300 s total deadline for response-header and
SSE waits, retaining connect/write bounds, with fixed-stage safe failure logs
and no retries. Its 31 relay and 11 setup tests passed. The existing four-file
relay ZIP was refreshed and verified; the user confirmed the updated Studio relay is running. Firmware still independently limits a no-data stall to 60 s.


Radio fix hardware result (2026-10-13): the corrected reader app is flashed at
0x10000, preserving the device filesystem. Image size 5,818,592 bytes, SHA-256
`bfbde286cf382961ee1b33823ad0dff8b706eed785d207806b3dcfff0703ccfa`,
app headroom 1,607,456 bytes. Compact `reader-candidate/` now contains this
image and matching compressed ELF/map; the pre-radio manifest is retained as
small provenance, and the saved pre-reader rollback bundle is unchanged.
The selected bootloader and partition table still match the earlier image.
The radio-fixed disabled-chat baseline also passed: `p4-glasses-test` image
5,469,696 bytes, with 1,956,352 bytes free (26.3%). It was not flashed. Compact
configuration, dependency/override provenance and image hash are retained in
`work/g2-llm-reader/baseline-radio/`; the full compiler output is disposable.

Full boot capture `reader-candidate-boot-20261013T132141Z.log` shows successful
Wi-Fi/NTP startup, Bluetooth auto-start and both G2 temples connected, with no
SDIO 258 errors or BLE teardown failure. C6 status reports zero transport errors,
zero start failures and zero recoveries. A fresh 1,924-byte houseplant reply
completed in 17 seconds through the updated Studio relay, with HTTP READY and
both temples still up. The model omitted the requested end marker, but the
stream completed normally and no truncation notice appeared. A read-only BASIC
settings request populated the exact 2.3.2.14 pair profile, enabling the existing
ring-only hold route. No hold policy or glasses settings were changed.
The wearer confirmed full-width replies and forward/backward ring paging.
The plain ring hold reached the reader page handler and the microphone flow
submitted `speaking test speaking` as a new user turn in the same conversation.
OpenClaw returned the completed 26-byte `No response from OpenClaw.` fallback;
the HTTP backend was READY without an error and both temples remained connected,
with zero C6 errors/recoveries. This proves input/submission, not a successful
conversational answer. The exact raw completion for this new turn is not yet
known; do not assume the earlier greeting's proven `NO_REPLY` applies here.
The wearer then asked the plant follow-up and confirmed a visible answer.
The device records a sixth turn with a complete 342-byte answer to the spoken
question (STT: `well whether i'm over water in my plant`) in the same chat.
HTTP returned READY without error; both lenses remained connected, with zero
C6 transport errors, missed heartbeats or recoveries after about 15 minutes.
This physically qualifies hold → local STT → review/Done → Mingus → reader.
The microphone-test silence remains an upstream behavior with unconfirmed raw
completion. There is no established first-attempt failure rule. No automatic
retry or global Mingus/sandbox policy change was made.

Cleanup: the old attached `openclaw-current-firmware/hardwareone` worktree was
archived using the managed worktree operation. Its known-good XIAO image from
236589c9, matching compressed ELF/map, flash/layout/configuration inputs and
unique build logs are retained in `work/openclaw-build/xiao-qualified/`
(34,152,740 bytes; manifest verified). The old checkout no longer exists; it
previously occupied approximately 1.68 GiB allocated. Both P4 builds and the
wearer qualification are complete. The donor's sole patched Arduino 3.3.5 tree
was copied to main `components/arduino/` and all 2,209 files verified (75,391,878
source bytes); it is the canonical build dependency, not another trial snapshot.
Both corrected candidate and baseline preserve the actual resolved P4 dependency
lock and repository seed. Unique tiny SDK-export logs were retained. The donor
had no active processes and its managed archive completed; the checkout no longer
exists. It occupied 2,965,644 KiB allocated before archive. Filesystem available
space increased from 22,248,868 to 24,571,652 KiB during removal (about 2.22 GiB
net, which can differ from directory allocation because of APFS/shared files).
