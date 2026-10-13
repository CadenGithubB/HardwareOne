# Food audio investigation — 2026-10-11

## Latest status: scrolling confirmed; positive diarization feedback

All eight Food turns were present in the previous saved replay; the unknown “and some chips” was not lost, and the final “oh” was user speech. The wearer reproduced scrolling failure on the new live layout. Successful CREATE/UPDATE replies continued but no navigation DevEvents arrived. The page used zero capturing children; official SDK rules require exactly one. Prior Q used two capture owners and the old failed live page used nine children, so both were confounded evidence for a blanket capture ban.

Current correction: only the existing one-line header captures input; the four-child shape, eight transcript rows and flicker-free band updates are unchanged. Host page, wire and decoded gesture routing sanitizer checks pass, including zero-capture regression. Both builds pass; selected app 5,460,320B has 917,152B actual headroom. App-only flash/expected boot/setup verified, no panic in35s. Current ELF `d4e15f5bc`, app SHA`f6357af11b335eacab587bf54e067b2ff9ff2dc996b90201ff23e9f8fd90e66f`. Physical retest accepted the page and delivered 10 header scrolls to previous/next handlers; 32 text writes and 32 TextSuccess replies followed with no page teardown. Wearer confirmed on 11 October 2026: “This works well. The diarization works great.” The scroll fix is visually qualified; diarization feedback is positive for this replay, without a new quantitative accuracy score. Passive observer stopped. No STT/model/LM policy change or training. Report: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/scroll-input-fix/RESULTS.md`. Login cleared by reboot; do not bypass normal authentication.

## Preceding status: unassigned phrase display fix installed

Latest saved transcript read succeeded after user-completed normal interactive login. All eight Food turns are present in order; “and some chips” is still unassigned, the other seven turns alternate Persons 1/2, and no third profile appears in this replay. The trailing “oh” is the user's speech, not a model hallucination. Evidence: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/unassigned-phrase-lines/latest-replay/RESULTS.md` and `transcript.txt`. No host diagnostic logger/raw capture ran for this replay.

The user now suspects the unknown phrase moved offscreen during late labeling and reports that scrolling stopped working after the flicker-free multi-text live page change. Investigate actual device-event routing to the navigation callback; existing direct-callback pagination tests do not cover that path. Read-only review found no deterministic middle-line deletion. Live view retains the newest eight wrapped rows including drafts, and its bands preserve line order. A separate unproven UPDATE_TEXT acknowledgment/cache recovery gap is not established as the cause and remains outside this bounded scrolling fix.

Normal login resolved the earlier approval-review rejection of a reuse-only read; subsequent authenticated transcript read succeeded. Do not use stored credentials or bypass console login after another reboot.

Current ELF prefix `289456903`; app SHA-256 `64b9932029fa8e4771d40d4e90bb740a82e71cfc64ac9b2139067bd135785a88`. Unknown phrases now start separate lines in the G2 live transcription view; only equal known speaker IDs coalesce. Late labels, drafts, receipt stitching and navigation remain covered by passing actual-source/wire sanitizer tests. Both firmware builds pass; application-only flash hash and expected boot/setup verified, no panic in35s. Physical lens appearance has not been rechecked.

No STT/backend/LM/profile-policy changes in this follow-up. LM remains1.0 and the previous live replay findings below still apply. Retained-history limit remains16 phrase records; evicted unknown prefix behavior is unchanged. Report, production diff and build/deployment evidence: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/unassigned-phrase-lines/RESULTS.md`. No passive serial logger is running.


## Speaker queue and phrase diagnostics — preceding build

Live replay update: all eight speaker jobs completed with zero skips. Turn 8 matched/saved Person 2 (.808). Turn 3 was unknown (.582 below .600), turn 5 seeded duplicate Person 3 (.570 vs Person 1), and turn 7 was ambiguous between Persons 3/1 (.726/.615). Saved lines 2–8 show completed label handling, including explicit unknowns; waits up to17.122s no longer expire at10s. No policy changes were made after this observation. The next bounded test should investigate provisional multi-observation profiles and stronger voice evidence, not tune thresholds to one clip. Within-phrase speaker-change detection remains unimplemented.

The replay started late in the first observation window, leaving an8.414s serial-log reconnect gap. Detailed final inputs1 and5–8 were observed intact, not all eight; final counters prove8processed/0skipped. All23 observed LM calls were strength1.0 and history_start0. Feature-cache end reports0overruns,9finals; final9 appears blank and had no speaker job. Later device activity was excluded and logger stopped. No new raw audio or saved transcript text was fetched; WER remains unmeasured. Results: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/cross-phrase-debug/live-replay/RESULTS.md`.

The follow-up application is installed and boot-verified (ELF prefix `0b9642024`). Original LM strength1.0 remains selected; the reduced0.75 trial was never flashed. The pinned15x5 gen2 model is unchanged; no training ran.

Added metadata-only `TL lm_decode`, `TL spk_match`, and `TL boundary` logs. Host regression confirms every LM decode starts with zero previous-phrase words. Session speaker profiles persist, but one label still covers a whole phrase: speaker handoffs inside a phrase are NOT detected, and no word/token timing is exposed. Current centered speaker crops can contain mixed voices. Slower labeling alone does not solve this.

Speaker admission now waits up to30s, yields under audio pressure, and records terminal unknown for skipped/failed work. Saved-label grace60s; normal drain30s. Worker joins before model/queue free and final counters. Fixed blank-result sequence advancement without blank transcript lines. Four PCM jobs, eight pending saved lines and16 live history entries remain bounded. Prepared-input packing and provisional multi-observation voice profiles are proposals only.

Validation: both final builds pass;123 queue sanitizer checks,788 cluster checks,24,000 policy/centroid parity comparisons; continuous metadata+PCM/cache checks pass; LM normal/soft-double suites and6 reference fixtures pass. Selected app5,460,320B fits actual slot with917,152B spare; baseline6,655,616B build-only. Flash hash verified; expected boot/setup complete; no panic in35s. Real-audio text/label accuracy is still unqualified. Serial login remains cleared by reboot; do not bypass authentication.

Report: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/CROSS_PHRASE_DEBUGGING.md`. Build/test/deployment evidence: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/cross-phrase-debug`. Current app SHA-256 `05f97ef54de6825efe73e92f67f7680deaee028ab0dc3dfc8edf069c903c3692`; rollback and private boot logs: `/Users/cadbecaimacmini/Documents/Codex/Scratch/hardwareone-p4-backup-20261011/cross-phrase-1791753486426669000`. The controlled Food replay is complete; a separate short-gap two-speaker benchmark and stronger-profile experiment remain future work.


Latest user request was to capture glasses audio alongside HardwareOne Transcription and diagnose possible chopped words. That baseline test is complete. A later full-tail firmware fix has now been installed; see the qualification update below. No model training ran. The user started the Food clip after its opening announcement.

Durable artifacts: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/REPORT.md` and `received.wav`. The louder `dialogue-listening-copy.wav` is playback only (+27.6 dB constant gain, pre-dialogue lead-in omitted); all measurements use original-level PCM.

Evidence: 595 complete LC3 packets, zero sequence gaps/duplicates/PLC/decoder errors/recorder drops, 29.75 s. All eight dialogue turns align to the public reference. Production segmenter replay matches all seven complete device TL boundary/trim records exactly, plus the remaining turn's partial log. Removed tails are near background. Quiet phoneme loss cannot be conclusively excluded. Low speech amplitude is observed, not established as the cause.

Correct Food transcript is `/sd/stt/u1/boot-local-e839d81100000004.txt`; preceding file03 was a longer session and is excluded from the report. The eight Food turns score 4 edits/51 words (7.84% WER) against the published script. Speaker IDs1,2,1,2,3,2,3,2 still split the male voice. No claim of accuracy improvement from code changes.

Source changes from this investigation, now included in the installed full-tail build:
- System_STT.cpp: suppress successful blank/whitespace finals from saved text/speaker jobs, retain completion count and draft-clear wake.
- stt/speaker_cluster.h: conservative unknown handling, minimum duration for first profile, no short-sample centroid updates, invalid embedding rejection. Coverage/real-world benefit remains unqualified.
- Focused host tests: continuous harness passes ASan/UBSan with and without --speakers; speaker cluster header passes327checks ASan/UBSan.

Preserve other preexisting checkout changes. Do not treat the prior speaker export evaluation as validating the new cluster policy. User permits increased latency. Training remains outside this task.

## Full-tail fix and qualification

Qualification update — full phrase retention, 11 October 2026

The baseline capture in STT_FOOD_AUDIO_INVESTIGATION_20261011.md remains unchanged. The new default sends the complete captured phrase to final STT instead of discarding samples after the detected voiced end plus 200 ms. Speculative finals are disabled with trimming; early drafts and capture-time feature caching remain enabled. Phrase segmentation and its 10-second limit are unchanged. The legacy trim/speculation policy remains opt-in for controlled comparisons.

Replaying the saved Food recording produces the same eight phrase boundaries, with no hard-limit cuts. Every sample within each full phrase is preserved as a byte-exact slice of the host-decoded recording. This restores 5.41 seconds total: 0.79, 0.78, 0.76, 0.80, 0.38, 0.40, 0.78 and 0.72 seconds across the eight turns. A/B playback uses the same constant gain and 0.5-second separators; its legacy version matches the previous listening copy exactly and neither version clips.

This establishes input preservation, not improved recognition. No model inference was run for this comparison, and host-decoded PCM has not been proven byte-identical to the P4 decoder. The existing 7.84% baseline WER and observed speaker split remain baseline results. The changed speaker policy still needs real-audio qualification. Longer final latency and changed whole-phrase normalization are expected consequences; their recognition impact remains unmeasured.

Validation reported by the test agent and coordinating agent:

- All three continuous-broker variants passed ASan/UBSan: default full phrase with speakers, default full phrase without speakers, and legacy trim/speculation with speakers (`--trim-silence`). The latest default speaker-enabled run exercised 97 cached finals and 99,391 feature rows with zero mismatches.
- The focused runtime checks passed. Speaker clustering passed 327 checks under ASan/UBSan.
- Both required firmware builds passed.

Selected image: 5,455,568 bytes; SHA-256 fb58bef08a7e33f2b7cfbc8ddb0279dbc07ec352d9d312ed6d52f1421a3fbf3b. The actual application slot is 0x615000 bytes, leaving 921,904 bytes of headroom. ELF SHA-256: 7537518f9ea2b7cf022836cc78b700ae3ca37681500742117f1a5dac7176e084.

Deployment completed: application-only write at 0x10000, flash data hash verified, expected ELF prefix 7537518f9 observed at boot, SDMMC mounted, setup completed, and no panic in the 35-second boot observation. Existing partition table, bootloader, storage, model files and settings were preserved. The selected profile still uses the pinned 15x5 gen2 model. The baseline build is verification only and was not installed because it exceeds this device's actual app slot.

Post-update live Food test completed: all eight final phrases logged captured=retained=inference, trim=0, ok=1 and lm=1. Recognition queue delay was 3–19 ms and processing took 623–1,622 ms per phrase. Minimum observed PSRAM was 3,182,440 bytes; minimum internal RAM was 127,264 bytes. Cached features covered 2,016/2,550 rows (79.06%). No panic was observed. This confirms the trimming fix on hardware; it does not measure WER.

The user reports mixed text/speaker results and the last label remaining "?". Diagnostics identify a separate pre-existing queue policy: speaker jobs 6 and 8 were skipped at backlog=4. Speaker lag reached 20.35 seconds; saved lines 2–5 exceeded their 10-second label wait. A follow-up correction is in progress: bounded cancellation-aware waiting for speaker capacity, longer saved-label grace, and safe draining. Speaker clustering can still return unknown for genuinely ambiguous evidence. Increasing fingerprint quality is a separate follow-up under evaluation.

The reboot cleared the serial console login, so the post-update saved text has not been read or scored. No recognition or speaker-label improvement is claimed. No model training ran.

Durable A/B audio and qualification: `/Users/cadbecaimacmini/Documents/Codex/Outputs/hardwareone-food-audio-20261011/full-tail-fix/`. The active firmware checkout remains `/Users/cadbecaimacmini/Documents/ChatGPT/HardwareOne-stt-speedups`, branch `stt-speedups`. Other checkout changes were preserved.
