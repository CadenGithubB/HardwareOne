# P4X-EYE microphone path and speech detector: bench investigation — 2026-11-07

Branch `claude/p4-sttraw-capture` (recorder build b2b81031 plus the tuning knobs below). Question: which
microphone-path and speech-detector settings reduce caption errors on the P4X-EYE, measured by caption WER on a
fixed playlist, without changing the recognition model or the speaker code. Everything below was measured on the
lab test P4X-EYE through the Mac mini's speaker at bench volume 70, the board about 1 ft away.

Tooling (STT repo, `tools/sttraw_ab/`): `make_playlist.py` builds the `ab1` playlist (616 s, 61 clips: read
speech loud and quiet, synthesized speech, AMI meeting excerpts, noise, silence); `score.py` aligns a raw
recording to the playlist, replays the production segmenter (`stt_raw_replay`, host build of
`stt/stt_continuous_config.h`) per arm and decodes every segment with one host decoder (int8 QuartzNet
gen2-25397 + meeting.lm), so differences between arms come only from the detector or the recorded audio.
Recordings come from the `sttraw` recorder (raw segmenter input, s16le 16 kHz, `/sd/STT Raw/`), pulled over
HTTP. Audio stays on the bench drive, never in the repository.

## 1. Noise-floor matrix (recorder build, no speech)

60 s windows, LCD off unless noted, cpu 400 MHz, 0 dropped samples in all ten recordings.

| condition | P50 frame RMS (counts) | P50 dBFS |
|---|---|---|
| quiet x3, LCD on x2, Mac fans (8 `yes` loops) x2, 06:49-07:15 | 5 | -76 |
| mic port taped x2, open control x1, 10:08-10:43 | 13 | -68 |

- Display, SD reads (model-load burst) and 20 s of Mac CPU load add nothing measurable (within 1 dB).
- Of the four tones seen in the first recording (43.9 / 120 / 240 / 3131 Hz), only 3131 Hz reproduces. It is
  at the same level in every recording, taped or open, while the tape did cut the surrounding 2.5-4 kHz band
  by 7 dB: internal (electrical/digital), not acoustic. Power ~-118 dBFS, far below anything the detector or
  model sees. No fix needed. 44 Hz was borderline in 2 of 20 windows; 120/240 Hz never appeared.
- The floor rose 8 dB between the early and the late recordings and the tape did not change it: steady
  (per-second RMS within +-1 count), broadband 100 Hz-1.6 kHz, no line structure. Thin tape is acoustically
  transparent below ~1.5 kHz, so the tape test cannot separate acoustic from electrical for this rise. The
  timing matched the Mac mini going under sustained load (screen sharing), which suggested its fan; the position
  arms later refuted that (the mic in contact with the Mac's chassis read 12.6 counts, lower than at 1 ft). The
  rise is unexplained (time of day, building); a putty seal or a quiet-time repeat would separate acoustic from
  electrical.
- Not done: Wi-Fi traffic arm (the saved network was unreachable; see the crash below), camera and camera+SD
  arms (camera not detected on this unit).

## 2. Detector A/B on one ab1 recording (production build settings)

Recording `raw_b989a8dc`, 632.8 s, 0 dropped. Room at 15 counts (Mac under load). Alignment: offset 78084
samples, drift 13.4 ppm, max residual 4.7 ms. Cells: WER % (deletions %) clips with no caption.

| arm | jobs/min | ls-test | ls-quiet | syn-ord | syn-quiet | ami-one | ami-two | all WER | false captions |
|---|---|---|---|---|---|---|---|---|---|
| production (floor 45) | 4.7 | 24.0 (7) 0/10 | 100 (100) 8/8 | 36.7 (23) 0/4 | 96.5 (94) 5/8 | 91.5 (80) 6/16 | 88.9 (67) 1/6 | 76.7 | 0 |
| floor 36 | 5.7 | 23.5 (6) 0/10 | 93.3 (91) 6/8 | 43.0 (18) 0/4 | 98.8 (95) 5/8 | 88.3 (72) 3/16 | 91.7 (61) 0/6 | 75.4 | 0 |
| floor 30 | 6.6 | 24.6 (8) 0/10 | 92.8 (89) 4/8 | 35.4 (18) 0/4 | 93.0 (88) 3/8 | 86.2 (74) 4/16 | 91.7 (69) 0/6 | 73.3 | 0 |
| floor 24 | 6.9 | 24.6 (8) 0/10 | 88.0 (83) 3/8 | 35.4 (18) 0/4 | 93.0 (90) 4/8 | 85.4 (70) 4/16 | 91.7 (68) 0/6 | 72.1 | 0 |
| floor 20 | 6.8 | same as floor 24 | | | | | | 72.1 | 0 |

Clean-source ceiling, same decoder: ls-test 5.5, ls-quiet 3.4, syn-ord 6.3, syn-quiet 8.8, ami-one 42.9,
ami-two 80.6.

Recorded levels (20 ms frame RMS, counts, median over clips): room 15 (P90 19-20); ls-quiet P50 17 / P90 23;
syn-quiet 17 / 24; ami-one 21 / 30; ami-two 22 / 37; syn-ord 20 / 36; ls-test 22 / 54.

Findings:

1. Lowering the speech floor is safe in this room (zero false captions on noise, silence or unassigned at
   every floor down to 20) and recovers little (76.7 -> 72.1). Floors 24 and 20 are identical because the
   segmenter opens at max(2 x room RMS, floor) and 2 x 15 = 30.
2. The misses are a level problem. Quiet speech peaks 4 dB above the room and cannot trigger the onset rule;
   when a louder syllable opens a segment the rest is cut (deletions ~90%). At production's 45-count floor
   only speech ~10 dB above this room is captioned at all.
3. The mic path has no headroom: the PDM slot ran IDF defaults (`amplify_num` 1 of 1..15, high-pass 35.5 Hz)
   and the saved `micgain` is applied in the ESP-SR/microphone path but not in STT's raw-HAL path. The STT
   chain worked on 20-50 counts of 32767.
4. The room floor (5 counts at a quiet time vs 15 with the Mac under load) is the largest single variable:
   10 dB, more than any setting.

## 3. Mic-path arms (tuning knobs, this branch)

Added as persisted settings with commands, applied at the next capture / STT session start, no reboot:

| setting | command | range | default | group |
|---|---|---|---|---|
| `pdmAmplify` | `pdmamplify` | 1-15 | 1 | mic (`System_Microphone`, HAL `audioSetPdmTuning`) |
| `pdmHpfHz` | `pdmhpf` | 0 = off, 23-185 Hz | 36 (= IDF 35.5) | mic |
| `sttspeechfloor` | `sttspeechfloor` | 1-32767 | 45 | stt (`System_Settings` stt group) |
| `sttsilencefloor` | `sttsilencefloor` | 1-32767 | 16 | stt |
| `sttnoisepercentile` | `sttnoisepercentile` | 0-100 | 25 | stt |

`sttdetector` prints the values in effect; `stt status` and every `sttraw` sidecar carry them. The PDM
pair configures the physical channel (every onboard-mic consumer); the detector trio is read by
`continuousSegmenterConfig` at session start, whose defaults are unchanged so host replay stays identical.

Arms recorded on 2026-11-07, 12:16-13:31, one ab1 playlist each, P4 ~1 ft from the Mac mini speaker, Mac under
load (screen sharing). Production detector unless stated; cells: WER % (deletions %) clips with no caption.

| arm | room P50 (counts) | jobs/min | ls-test | ls-quiet | syn-ord | ami-one | all WER | false captions |
|---|---|---|---|---|---|---|---|---|
| x1, HPF 36 (= today's production) | 18.8 | 4.1 | 33.3 (14.8) 0/10 | 98.1 (96) 6/8 | 76.0 (63) 1/4 | 97.2 (91) 9/16 | 83.8 | 0 |
| x4 | 70 (17.6/gain) | 6.0 | 26.2 (9.3) 0/10 | 100 (96) 5/8 | 44.3 (25) 0/4 | 93.5 (85) 8/16 | 79.3 | 0 |
| x8 | 117 (14.6/gain) | 6.2 | 17.5 (3.8) 0/10 | 94.2 (91) 4/8 | 30.4 (11) 0/4 | 89.5 (72) 4/16 | 73.4 | 0 |
| x1, HPF 120 | 13.1 | 4.9 | 18.0 (3.8) 0/10 | 100 (100) 8/8 | 40.5 (20) 0/4 | 87.5 (73) 5/16 | 74.2 | 0 |
| x1, HPF 185 | 13.4 | 4.8 | 20.2 (3.8) 0/10 | 100 (100) 8/8 | 40.5 (20) 0/4 | 87.8 (73) 7/16 | 76.2 | 0 |
| (morning) x1, HPF 36, room 15 | 15.1 | 4.7 | 24.0 (7.1) 0/10 | 100 (100) 8/8 | 36.7 (23) 0/4 | 91.5 (80) 6/16 | 76.7 | 0 |

Offline floor arms (36/30/24) on the same recordings: at x4 and x8 they change nothing (2 x room RMS is above
every floor); at x1 with HPF 120/185 they add 2-3 points (74.2 -> 70.8, 76.2 -> 72.1). Zero false captions in
every arm, including with the room at 117 counts (x8), well above the fixed 45 floor: the adaptive rule holds.

What the arms say:

- **Gain (x1 -> x8) helps monotonically on everything loud enough to be heard**: loud read speech 33 -> 17.5 WER
  with deletions 15 -> 4 (phrases no longer cut short), meetings 9 -> 4 uncaptioned of 16. Room, speech and
  quiet-speech levels scale exactly with the gain (per-gain room 15-19 counts in all arms), so the ratio quiet
  speech / room stays 3.1-3.5 dB and quiet speech stays mostly lost at any gain: that is the room, not the chain.
- **The 120 Hz high-pass alone is worth about as much as x8** (74.2 vs 73.4) and for a different reason: it
  removes 3 dB of the room (18.8 -> 13.1 counts; a third of the room power sat below 100 Hz) while loud speech
  loses nothing, so the adaptive onset threshold (2 x room) falls from ~37 to ~26 and phrases stop being cut.
  185 Hz is slightly worse than 120 (speech starts to lose energy). The two levers act on different things
  (resolution vs threshold) and should stack; the combination was not recorded at this position.
- **Run-to-run spread is large**: two x1 recordings two hours apart scored 76.7 and 83.8 (room 15 vs 19 counts).
  Differences under ~7 points between single recordings are not reliable; the x1 -> x8 and x1 -> HPF 120 gaps
  (10 points, same hour, same room) are.
- Device captions during the arms (saved per arm): x1 38, x4 47, x8 58, HPF 120 45, HPF 185 45; more and
  longer captions with gain, matching the host scores.

### Position arms: P4 resting against the Mac mini (speaker distance ~0), 13:44-14:19

Same playlist, same volume, same build and settings; only the board moved from ~1 ft away to resting on the side
of the (upright) Mac mini, mic side unchanged.

| arm | room P50 | speech above room (loud / quiet, dB) | jobs/min | ls-test | ls-quiet | syn-ord | ami-one | ami-two | all WER | false |
|---|---|---|---|---|---|---|---|---|---|---|
| x1 | 12.6 | 22.4 / 11.1 | 7.3 | 8.7 (0.5) 0/10 | 28.4 (12) 0/8 | 11.4 (1.3) 0/4 | 68.0 (34) 2/16 | 80.6 (47) 0/6 | 37.1 | 0 |
| x4 | 52 (12.9/gain) | 22.2 / 10.8 | 8.1 | 7.7 (1.1) 0/10 | 19.7 (4.8) 0/8 | 13.9 (2.5) 0/4 | 70.5 (37) 3/16 | 86.1 (38) 0/6 | 35.9 | 0 |
| x8 | 104 (13.0/gain) | 22.2 / 10.8 | 8.1 | 9.3 (0.5) 0/10 | 23.6 (5.3) 0/8 | 11.4 (1.3) 0/4 | 70.5 (39) 2/16 | 84.7 (32) 0/6 | 36.6 | 0 |
| (1 ft) x1 | 18.8 | 9.2 / 3.1 | 4.1 | 33.3 (15) 0/10 | 98.1 (96) 6/8 | 76.0 (63) 1/4 | 97.2 (91) 9/16 | 95.8 (76) 2/6 | 83.8 | 0 |

Clean-source ceiling: ls-test 5.5, ls-quiet 3.4, syn-ord 6.3, ami-one 42.9, ami-two 80.6.

- **Distance is the whole story.** Speech at the mic rose 10 dB (loud read speech 54 -> 166 counts at x1); quiet
  speech went from 3 dB to 11 dB above the room and is now captioned every time (0 of 8 missed, WER 28% from
  98%). Read speech sits 3 points above the clean-source ceiling (8.7 vs 5.5), synthesized speech 5 points
  (11.4 vs 6.3). Meetings (AMI) stay at 68-86%: that is the material (ceiling 43/81), not the mic.
- **Gain does nothing once the level is adequate**: x1/x4/x8 at 37.1/35.9/36.6 are within the run-to-run noise.
  Gain only mattered when speech was 20-50 counts, i.e. at distance. Still zero false captions at x8 with the
  room at 104 counts.
- **The Mac mini's fan is not the room noise.** With the mic in contact with the chassis the room floor read
  12.6 counts, lower than the 15-19 measured a foot away earlier in the day. The 06:50 (5 counts) vs 10:00+
  (13-19 counts) difference in the noise-floor matrix therefore remains unexplained (time of day / building);
  it is not the Mac.
- Device captions per arm: 68 / 70 / 71 (vs 38 / 47 / 58 at 1 ft).

Combination arms at the same position (14:38-15:01): x1 + HPF 120 -> all WER 36.8 (ls-test 10.4, ls-quiet 22.1,
syn-ord 11.4, ami-one 69.2, 0 false); x8 + HPF 120 -> 35.3 (10.4 / 18.8 / 8.9 / 69.6, 0 false). Five arms at this
position span 35.3-37.1: once speech is ~10 dB above the room, neither gain nor the high-pass changes the result.

## Conclusions

1. The P4X-EYE mic path adds no measurable noise of its own (display, SD, tones) and reaches within ~3 points of
   the clean-source ceiling on read speech when speech is ~20 dB above the room. What limits captions in use is
   the speech level at the mic, i.e. mouth-to-mic distance and the room.
2. At marginal levels (speech 10-20 dB over the room, as at 1 ft from a speaker) two settings help and cost
   nothing: **`pdmhpf 120`** (room -3 dB, onset threshold down, deletions 15% -> 4%, all WER 83.8 -> 74.2) and
   **`pdmamplify 8`** (resolution; 83.8 -> 73.4). They act on different things; the combination was only recorded
   at the close position, where nothing matters. Recommended default for the glasses build to evaluate next:
   HPF 120, amplify 4-8 (8 leaves ~30x headroom for shouting), floors unchanged.
3. Lowering the speech floor (36/30/24/20) never produced a false caption in 11 recordings but only helps when the
   room is quiet enough that 2 x room < floor; with HPF 120 it adds 2-3 points. The 2 x room onset multiplier is
   the rule that actually gates quiet speech; it is not a setting yet.
4. Not changed, as the brief required: the recognition model, the speaker code, the hardware.

## 4. Live check

Replaced, at the user's request, by the speaker playlist at two positions (above). No live test was run.

## 5. Offline: a tilt-based onset judge (proposal, not implemented)

The loudness rule (open at 2 x room RMS) cannot see quiet speech 3-4 dB above the room. Feeding the segmenter a
pre-emphasized copy of the audio (y[n] = x[n] - 0.97 x[n-1], one multiply-add per sample) for its DECISIONS only,
normalised to the room level so the fixed floors keep their meaning, while the model still receives the raw
samples, was scored offline on the eleven recordings above (STT repo `private/sttraw-bench/RESULTS-vad.md`):

| 1 ft recordings (6) | loudness judge | pre-emphasis judge |
|---|---|---|
| all WER | 73.4-83.8 | 56.2-65.4 (every recording 14-20 points better) |
| quiet read speech uncaptioned | 4-8 of 8 | 0-1 of 8 |
| false captions on 6 noise clips + silences | 0 | 0 (one single-word segment in one recording) |
| captions per minute | 4.1-4.9 | 7.0-8.3 |
| close-position controls (2) | 36.6 / 37.1 | 36.7 / 37.2 (unchanged) |

Costs: judged on one room's bass-heavy hum (a bright-noise environment needs testing before shipping), rescued
speech decodes at 55-80% WER (more text, more of it wrong), ~1.7x more decode jobs at distance, floors must be
specified in the decision signal's units, higher sensitivity also catches bystanders. Software AGC was also
bounded: digital x8 recovers ~60% of hardware x8's benefit (floors regaining meaning); not worth building beyond a
fixed `pdmamplify 8` + `pdmhpf 120`.

Implemented on branch `claude/stt-decision-pre`: `SegmenterConfig::decision_pre_emphasis` (x100, default 0; the
per-sample pre-emphasis is accumulated beside the raw frame sums and its level is scaled by a gain learned from the
500 ms calibration window, clamped x1/8..x64), `continuousSegmenterConfig(..., decisionPreEmphasis)`, the `stt`
setting/command `sttdecisionpre` (0..99), fields in `stt status`, `sttdetector` and the raw sidecar, and
`--decision-pre N` in `stt_raw_replay`. Host suites pass. Scored with the real segmenter at 97 on the eleven
recordings: 1 ft all-WER 76.7/83.8/79.3/73.4/74.2/76.2 -> 56.2/62.7/66.2/57.1/59.7/59.1 (quiet read speech missed
4-8 of 8 -> 0-1 of 8), close-position 35.3-37.1 -> 34.2-38.3 (noise), one false single-word caption on silence in
one recording. On the device (18:20-18:46, board still resting on the Mac mini): `sttdecisionpre 0` then `97`,
knobs read back correctly, 0 dropped samples, no panic, device captions 67 -> 83; at that close position the
host score is unchanged (36.0 / 38.0), as predicted. Still to do: the decisive device arm at ~1 ft with
`sttdecisionpre 97` (+ `pdmamplify 8`, `pdmhpf 120`) and a bright-noise recording before it becomes a default.

## 4. Live check with the user

_Pending._

## Other findings

- **Three panics in one day, two kinds** (bench report `private/sttraw-bench/PANIC_INVESTIGATION.md` in the
  STT repo, from a read-only code investigation with adversarial verification; firmware not changed):
  - (A) `radiopower on`, ~8 s, `stt start` -> Guru Meditation core 0, MEPC 0x40008950 / MTVAL 0x34. The dump
    decodes to IDF `esp_vfs_stat` on the **main loop task** appending the 5 s battery sensor log to
    `/littlefs/battery.csv`: `fopen` failed (logged just before), the VFS registry returned a **corrupt entry**
    (`vfs` pointer 0x8, entry pointer in PSRAM although `s_vfs` is internal .bss) and `->dir` at +0x2c faulted.
    A wild write into the VFS table after the C6 bring-up; STT's model open was a 17 ms coincidence on the other
    core. Writer not identified (neighbours in .bss: the PSRAM context, ESP-Hosted transport/channel structs).
  - (B) reset=panic ~7 s after `stt stop`, 2 of 6 long web-owned sessions (16:37:33 and 18:01:45 UTC), no dump
    captured. The recorder, the mic HAL and the stop ordering were checked and cleared (every task joined before
    its buffers are freed; sidecar `drain_complete` true). Best explanation: corruption made during the session
    that trips at teardown (the session's first ~10 large PSRAM frees, heap poisoning off) or at the next
    path-based VFS call, i.e. the same family as (A). Not proven.
  - Next: passive serial listener through stop + 30 s (bench driver does this now), `crashlog json` on the test
    build before any restore flash (also in the driver), a bench config with `CONFIG_HEAP_POISONING_LIGHT` and
    a flash coredump (check the p4x_eye partition table first), `heap_caps_check_integrity_all` at boot /
    after `radiopower on` / after the model read starts / at the three teardown points, and a `s_vfs[]` watch.
    Reproduction for (A): boot, `radiopower on`, 8 s, `stt start`, several times; then with `sensorlog stop`.
- Task watchdog register dumps (`task_wdt: IDLE0`, MCAUSE 0xdeadc0de) ~8 s after boot and right after a
  Wi-Fi join are warnings, not crashes (`CONFIG_ESP_TASK_WDT_PANIC` unset): `main` holds CPU 0 for 7-9 s
  (LOOPHEALTH IO stall). Worth a ticket, unrelated to the reboots.
- The "web session rejected (401) ~10.5 min after login" seen twice was the reboot from (B): the web server came
  back on its own and refused the old cookie.
- Bench workflow that worked: esptool only for the app-slot read/write (MAC, partition table and hash
  checked each time, production restored after every session); everything else over HTTP (`/login`,
  `POST /api/cli`, `GET /api/files/read?name=`), with `wifiautostart` and `httpAutoStart` on so the board
  returns to the network by itself after a flash. A 20 MB recording pulls in ~46 s.

## Not verified

- One recording per arm, one room state; no quiet-time repeat.
- Acoustic vs electrical for the 8 dB floor rise (tape was transparent below 1.5 kHz).
- Scores are host-decoder replays, not the device's own captions; device captions of the first ab1 run were
  lost to the 401, later runs keep them.
- Segmenter timing knobs (end silence, min phrase, blip tolerance) and the hard-coded 2x onset multiplier
  were not varied.

## 6. Device arms at 1 ft (2026-11-08)

Build `fe09e51e` (with the P4 clock-switch fix and the fixes below), profile `p4-openclaw`, test P4X-EYE about 1 ft from the Mac mini speaker, volume 70, debug logging
(`sttperf log on`) and a passive serial log in every arm. Each arm sets all knobs, so arms do not inherit
from each other. The kept app slot (g2-health-push) was restored and hash-verified after each run. Scoring
uses the replay build of the same source and one host decoder (int8 QuartzNet gen2-25397 + meeting.lm). Every arm's
replay reproduces the device's own `TL boundary` lines exactly (40/40, 76/76, 93/93, 75/75, 43/43).
Scorer change: a segment that overlaps a speech clip by at least 0.3 s is that clip's caption, even when
it opened early in a preceding silence (one such phrase had been counted as a false caption).

**ab1, 5 interleaved arms, 06:40-07:43.** All WER %, quiet read speech clips with no caption out of 8.
Each recording is scored with both detectors; `*` marks the detector the device ran.

| recording (device settings) | room P50 (counts per gain) | loudness judge | pre-emphasis judge 97 | quiet missed (loudness / judge) |
|---|---|---|---|---|
| base (x1, HPF 35.5) | 15.3 | *74.9 | 54.0 | 8 / 0 |
| judge97 (x1, HPF 35.5) | 13.9 | 75.1 | *56.6 | 8 / 1 |
| combo (x8, HPF 120) | 10.9 | 71.5 | *54.7 | 6 / 0 |
| x8h120 (x8, HPF 120) | 9.5 | *57.6 | 44.5 | 2 / 0 |
| base2 (x1, HPF 35.5) | 11.5 | *72.3 | 47.5 | 7 / 1 |

Clean-source ceiling, same decoder: ls-test 5.5, ls-quiet 3.4, syn-ord 6.3, syn-quiet 8.8, ami-one 42.9,
ami-two 80.6. False captions on noise, silence and between clips: 0 in all ten cells. Segments per minute:
3.8-4.1 (loudness, x1), 6.4-7.1 (loudness, x8 + HPF 120), 6.9-8.8 (judge).

- The judge is better on every recording, by 13-25 points, and recovers the quiet speech (missed 6-8 of 8
  -> 0-1 of 8). Loud read speech is unchanged or better (17.5 -> 16.9, 13.7 -> 14.2, 16.9 -> 12.6).
- x8 + HPF 120 helps too: the best cell is x8 + HPF 120 + judge (44.5). The two x8 + HPF 120 recordings
  differ by 14 points under the same detector (71.5 vs 57.6), with the room 1.2 dB apart: at 1 ft the
  onset rule sits on a knife edge, so single-recording differences under ~10 points are not reliable.
  Compare detectors within a recording, as in the table.
- The room fell from 15.3 to 11.5 counts during the run (Mac load; not controlled).

**nb1 bright noise, 3 arms, 07:53-08:21** (new playlist `tools/sttraw_ab/make_noise_playlist.py` in the STT
repo: white hiss at -40/-32 dBFS, pink -36, the six brightest DEMAND test environments and the four brightest
MUSAN noise clips at -36 dBFS, 10 s each; LibriSpeech over hiss and over cafeteria babble; three silences).

| recording | loudness judge WER (speech in noise) | judge 97 | false caption segments / words on 190 s of noise |
|---|---|---|---|
| base (x1) | *28.4 | 28.0 | loudness 1 / 3, judge 3 / 11 |
| judge97 (x1) | 28.4 | *29.9 | loudness 1 / 3, judge 4 / 7 |
| combo (x8, HPF 120) | 31.5 | *29.6 | loudness 2 / 4, judge 2 / 7 |

- No caption on white or pink hiss, on any MUSAN clip, or on station, river and park noise with either
  detector. The bright-noise concern for the judge did not appear.
- Nearly every false caption, with both detectors, is on DEMAND `OMEETING` (a recorded meeting room with
  people talking); the judge adds fragments there and one syllable each on cafeteria and restaurant noise.
  That is the expected cost: more sensitivity to bystanders.

**ab1 at Mac volume 85, 3 arms, 08:24-09:05** (the user found 70 quiet; roughly a talker at normal level, 1 ft).

| recording (device settings) | loudness judge | judge 97 | quiet missed (loudness / judge) |
|---|---|---|---|
| base (x1, HPF 35.5) | *56.6 | 37.6 | 3 / 0 |
| judge97 (x1, HPF 35.5) | 54.9 | *36.7 | 3 / 0 |
| combo (x8, HPF 120) | 42.4 | *38.5 | 0 / 0 |

Read speech 9.3-13.7 (ceiling 5.5), ami-one 70-74 (ceiling 42.9). Replays match the device exactly; one
one-word false caption on a noise clip (judge, base recording). At this level the judge is worth ~18 points
at x1 and x8 + HPF 120 adds little once the judge is on.

**Session stability.** Eleven 7.5-10.5 min continuous sessions on this build (ab1 x5, nb1 x3, ab1 at volume
85 x3): no panic, no dropped samples, no audio overrun, every session `done`. This covers the long-session
check left open in KNOWN_ISSUES for the clock fix.

**Saved tuning across a restart.** `pdmamplify 8`, `pdmhpf 120`, then a USB chip reset: the next session's
sidecar records x8 / 120 Hz (before the fix STT ran x1 / 35.5 Hz after any reboot, because only
`initMicrophone()` pushed the saved values to the HAL). `sttdetector` showed 1/36 after the restart because it
read the HAL; it now reports the saved settings.

**Warm-up "Cancelled" (KNOWN_ISSUES).** 20 fast start/stop cycles with the raw recorder armed: 19 done, 1
failed with `Microphone audio loss detected; session stopped` half a second after the start (this is what
logs `warm-up failed: Cancelled`). Raw file 0.0 s: the overrun happened after the zero baseline and before the
first read, while the bench recorder opened its SD file and the model file was read from the same card. The
PDM DMA ring is 256 ms and buffered-PCM trim is a no-op for PDM, so the strict loss rule failed the session for
audio that was never the session's. All failures seen so far (4/40 on 2026-11-07, 1/20 today) had `sttraw`
armed. Fix: the integrity baseline is re-taken just before the first read (`TL capture_preroll_overrun`
logs any pre-read drop); loss inside the session still fails it. Device verification: see below.

**Software reboot loses the C6 link** (not STT; handed to the release session as `claude/fix-c6-reboot`).
After `reboot` the P4 resets the C6 by GPIO9, the C6 shows only its ROM banner, and every SDIO command times
out (0x107); each 60 s companion retry then logs `Card init success` followed at once by `Failed to get
ESP_Hosted slave transport up`. HTTP stayed down for 6 minutes; a USB chip reset recovered it in 16 s.

## 7. Automatic mode: the shipped defaults (2026-11-08)

The user's requirement: the mic path should adjust itself, not depend on knobs, and the gain must not jump on
one session. What ships:

- **Detector:** `sttdecisionpre` defaults to 97 (the pre-emphasis onset judge). It calibrates itself from
  the room in each session's first 500 ms and tracks the room afterwards; `0` restores the loudness-only rule.
- **Mic for STT sessions (`sttautogain`, default on):** STT opens the onboard PDM channel with its own tuning
  (the channel is created per capture, so wake word and recordings keep the saved `pdmamplify` / `pdmhpf`):
  - high-pass 120 Hz;
  - gain from `stt/stt_auto_gain.h`, starting at x8 (the highest gain qualified on the bench);
  - every session that heard speech and lasted 2 s or more records the gain it would have wanted: the gain
    that puts its third-loudest 10 ms frame near -18 dBFS (rounded to the nearest step), or half the gain if
    more than 1 in 20,000 samples clipped;
  - the gain changes only when the last 3 recorded sessions all want a change in the same direction, then
    moves to the most cautious of their wishes, at most x2 or x0.5, and the record starts over;
  - the record lives in RAM: a reboot starts again at x8 with an empty record.
  - `sttautogain 0` makes STT use `pdmamplify` / `pdmhpf` like every other consumer.
- `sttdetector` and `stt status` report what the next session will use (`autoGain`, gain, high-pass, record
  size); each session logs `TL autogain used= wanted= next= history= loud3= clipped=`.

**Automatic vs today, ab1 at 1 ft, volume 70, interleaved (build 6ff54758, 09:48-10:39).**

| arm | mic | all WER | quiet read speech missed | loud read speech | segments/min | false captions |
|---|---|---|---|---|---|---|
| old (`sttautogain 0`, `sttdecisionpre 0`) | x1, 35.5 Hz | 72.0 | 8/8 | 16.9 | 4.1 | 0 |
| automatic | x8, 120 Hz | 44.3 | 0/8 | 14.2 | 7.7 | 0 |
| old | x1, 35.5 Hz | 72.2 | 8/8 | 15.8 | 4.4 | 0 |
| automatic | x8, 120 Hz | 48.0 | 0/8 | 14.8 | 7.9 | 0 |

Replays reproduce the device boundaries exactly (43/43, 82/82, 46/46, 83/83). Both automatic sessions logged
`used=8 wanted=8` (loud3 1468 / 1449: still 9 dB under the target, so x8 stays the cap).

**Coming down by itself: board resting against the Mac mini, volume 85, five consecutive 4-minute automatic
sessions in one boot.**

| session | used | wanted | next | loud3 |
|---|---|---|---|---|
| 1 | x8 | x5 | x8 (1 of 3) | 6396 |
| 2 | x8 | x5 | x8 (2 of 3) | 6434 |
| 3 | x8 | x5 | **x5** (agreed; record cleared) | 6483 |
| 4 | x5 | x4 | x5 (1 of 3) | 4130 |
| 5 | x5 | x4 | x5 (2 of 3) | 4109 |

Scores (judge on in all, first 240 s of ab1): x8 sessions 21.7 / 19.8 / 24.1, x5 sessions 20.4 / 24.4, and a
fixed-x1 control 22.0; loud read speech 0% in every arm, no clip uncaptioned. Coming down cost nothing. (Each
arm shows one 10-word "unassigned" segment: the clip that playback cut at 240 s, which the scorer excludes.)

No clipping in any session. At x5 the loud speech sat at the target (4130 vs 4096) but the wish rounded
down to x4, which would have crept one step further; the wish now rounds to the nearest step (x5 at 4130),
host-tested, not yet re-run on the device.

## 8. G2 glasses mic and per-mic detector profiles (2026-11-08)

The detector is source-agnostic, so the judge default applied to the G2 glasses mic untested. Measured the
same way (build e12ec586, `micsource g2`, `sttraw` records the G2 PCM the segmenter is fed; both detectors
replayed on each recording). The G2 stream is LC3 over BLE after the glasses' own AGC and drops ~0.5-0.6% of
its samples over 10 minutes, so the scorer's waveform alignment failed (2 of 18 windows); `score.py --align
envelope` (piecewise, on the 10 ms loudness envelope, re-anchored every 15 s) kept 37/37, 22/23 and 19/19
windows.

| glasses position | playlist | loudness judge | judge 97 | quiet read + synthetic missed (loudness / judge) | false captions (loudness / judge) |
|---|---|---|---|---|---|
| beside the Mac mini | ab1 | 30.3 | 30.7 | 0 / 0 of 16 | 0 / 1 word |
| 1 ft | ab1 | 72.2 | 69.0 | 13 / 5 of 16 | 0 / 1 word |
| 1 ft | nb1 | 36.8 (speech in noise) | 38.0 | - | 1 seg 3 words / 1 seg 1 word |

Close up the glasses are near the clean ceiling (read speech 7.1 vs 5.5). At 1 ft from a speaker they are no
better than the P4 mic: the G2 mic is made for the wearer's mouth, so this is a pessimistic case. The judge
is neutral close and in noise and helps at a distance, so it is the G2 default too.

**Per-mic profiles.** Each mic now has its own detector levels, chosen at session start for the mic the
session will use (native glasses stream or explicit request; otherwise the onboard mic first, the HAL's auto
order; a mismatch is logged as `TL detector_profile_mismatch`):
- onboard mic: `sttspeechfloor` 45, `sttsilencefloor` 16, `sttnoisepercentile` 25, `sttdecisionpre` 97,
  plus `sttautogain` (PDM boards only; ES7210 codec boards keep their codec gain);
- G2 glasses: `sttg2speechfloor` 45, `sttg2silencefloor` 16, `sttg2noisepercentile` 25, `sttg2decisionpre` 97.

`stt status` reports `detectorProfile` and the levels in use, `sttdetector` both profiles, and the raw
sidecar the profile the session ran with.
