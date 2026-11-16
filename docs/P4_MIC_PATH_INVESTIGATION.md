# P4 microphone path for captions: static investigation

7 November 2026. Code reading, datasheets and arithmetic only. No board was attached to this
Mac, `/Volumes/Audiodrive` was not mounted, and no saved P4 audio was available, so **nothing
here is a new measurement**. Evidence numbers come from the standalone STT project
(`CadenGithubB/ESP32_Onboard_Speech_To_Text`, branch `claude/spkeval-diagnostics`, commit
`9b18411`): `docs/STT_OVERLAP_GATE_PHASE4_PLAN.md` §13–17, the end of
`docs/STT_PRODUCT_HANDOFF.md`, `docs/STT_ADMISSION_THRESHOLD_AB.md` and
`docs/STT_DETECTOR_FOLLOWUP.md`. No code was ported and no firmware was changed.

## 1. HardwareOne's path matches the standalone example

| Stage | HardwareOne (P4X-EYE) | Standalone `p4_eye_captions` |
|---|---|---|
| Driver | I2S0 PDM RX, new channel API, `HAL_Audio.cpp:90-207` | same |
| Pins | CLK 22, DATA 21, power GPIO12 (`System_BuildConfig.h:1428-1435`) | same |
| Clock | 16 kHz, `DSR_16S` → 2.048 MHz, chosen by the band check at `HAL_Audio.cpp:137-146` | `DSR_16S` hard-coded |
| Slot | `PCM_FMT_DEFAULT`: mono left, 16-bit, `hp_en=true` 35.5 Hz, `amplify_num=1` (IDF v5.5.5 defaults, never overridden) | same |
| Warm-up | 3 × 1024 samples discarded (192 ms); no explicit delay after rail-on | 3 blocks discarded |
| Processing before STT | none: raw HAL PCM (`System_STT.cpp:494-495`) | none |
| Segmenter | `stt/stt_segmenter.h`, adaptive, floors 45 / 16, P25 noise, 30 ms blip, 4 s min, 10 s max, 1 s short close, 2 s quiet-cut search (`System_STT.cpp:1848-1876`) | identical settings (`main.cpp:479-485`); its header adds only the opt-in `freeze_noise_during_speech` |
| Model frontend | 1/32768, 0.97 pre-emphasis, log-mel, per-utterance CMVN, no dither | same |

So the standalone caption-WER results apply to HardwareOne's STT. They do not apply to the
Waveshare P4. Its capture goes through an ES7210 ADC with 37.5 dB analog gain
(`HAL_Audio_Waveshare.cpp`), a different path that has not been measured.

Two traps for measurement:
- `micrecord` does **not** record what STT hears. For the PDM source it applies DC removal, a
  ~50 Hz high-pass filter, 0.97 pre-emphasis and **24× gain** (`System_Microphone.cpp:758-819`).
  Its WAVs are about 27.6 dB hotter and tilted.
- HardwareOne cannot save STT input audio. The standalone `pcm save` saves only admitted segments,
  so the audio the detector rejected was never recorded on either night.

## 2. The microphone is operated correctly

- **Part.** MEMSensing MSM261DHP006 (U9, left-slot strap; `System_BuildConfig.h:1425-1432`,
  `experiments/audio_portable/README.md:98-103`). The LCSC listing gives −26 dB sensitivity, 64 dB
  SNR and 670 µA, the same figures as the sibling MSM261D3526H1CPM datasheet.
- **Sibling datasheet** (V1.2, Dec 2018):
  - clock bands: low power 150–900 kHz, standard 1.1–4.0 MHz;
  - standard mode: −26 dBFS at 94 dB SPL, SNR 64 dB(A), AOP 120 dB SPL, PSRR 50 dBV/FS at 1 kHz;
  - power-up time 6 ms typical, 20 ms max; mode change ≤ 10 ms.
  Get the DHP006's own datasheet (LCSC C22390138) to confirm.
- **Clock.** 2.048 MHz is inside the standard band. 1.024 MHz would fall in the 0.9–1.1 MHz gap,
  so `DSR_16S` is right. The 192 ms discard covers the 20 ms power-up.
- **Level is not a defect.** At −26 dBFS per 94 dB SPL, the sound level in dB SPL is about the
  dBFS reading plus 120 (±3 dB depending on the sine or RMS convention).
  - −60 dBFS speech is about 60 dB SPL at the mic: normal conversation at 1 m. Nothing is losing
    gain.
  - AMI's −43 dBFS reflects that corpus's own recording gain.
- **Self-noise is well below the observed floor.** 64 dB SNR puts the mic's own noise near
  −90 dBFS(A), about 30 dB(A) SPL or about 1 LSB RMS.
  - The −78 dBFS floor (about 42 dB SPL) is about 12 dB above the microphone's self-noise.
  - So the floor comes from the room (Mac mini fan, HVAC) or from electrical pickup, not from
    the microphone.
- **No meaningful bit loss.** 16-bit quantization noise is about −101 dBFS, roughly 11 dB below
  the mic's self-noise. It costs about 0.3 dB, and `amplify_num` cannot recover more than that.
  - A sealed-port capture at `amplify_num` 1 vs 4 settles whether the decimator truncates
    internally. If the floor rises about 12 dB, nothing was being lost.
- **Electrical suspect: the shared rail.** GPIO12 switches `3V3_PERI`, which also feeds the camera,
  LCD and IMU.
  - At 50 dB PSRR, about 40 mV RMS of audio-band ripple would reach −78 dBFS; 10 mV reaches −90.
  - Camera and LCD load steps on a shared LDO can reach that. Ripple would show as tones at frame
    or refresh rates and their harmonics. The 35.5 Hz high-pass filter removes only the lowest.
  - This is untested.

## 3. Why quiet talkers never become a segment

The detector works on raw RMS per 10 ms frame (`Audio_VadPolicy.h`, `stt_segmenter.h:255-290`).
- **Onset** needs one frame at or above `max(45, 2 × P25 noise)`.
- **After onset**, a frame counts as voiced at or above `max(16, 2 × noise)`. It counts as silent
  below `max(16, 2 × noise, peak/8)`.
- A phrase needs **200 ms of voiced frames** or it is discarded.

| Threshold | RMS | dBFS | ≈ dB SPL at mic |
|---|---|---|---|
| Onset floor `adaptive_speech_floor` | 45 | −57.2 | 63 |
| Voiced and silence floor `adaptive_silence_floor` | 16 | −66.2 | 54 |
| Ordinary replayed speech, median | ~33 | −60 | 60 |
| Quiet arm (−18 dB), median | ~4 | −78 | 42 |
| Room floor (quietest in-speech frames) | 3–4 | −78 to −81 | 39–42 |

- **The onset floor is above typical speech.** It is about 3 dB above the median of ordinary
  speech, so a phrase starts only on a stressed syllable.
- **Quiet speech never reaches either floor.** The −18 dB arm peaks around 15–20 RMS, below 45,
  so 64–66% of its clips never open.
- **In a quiet room the fixed floors decide everything.** There `2 × noise` is only 6–8 RMS, so
  the fixed floors set both thresholds. They were tuned for "measured P4 room noise (3..31)"
  (`stt_segmenter.h:28-29`), which includes noisy rooms, but the relative `2 × noise` term
  already covers noisy rooms.
- **The 16 floor also cuts words inside segments.** Word stretches between 4 and 16 RMS count as
  silence. That ends phrases early and keeps them from reaching 200 ms of voice. This fits the
  meeting-speech finding that most extra deletions are inside segments.

**What has already been tested** (standalone, saved six-second P4 clips, host replay of the real
C++ pipeline):
- *Lowering only the onset floor* (45 → 32/24/16, voiced floor fixed at 16) gave no quiet gain.
  Background output rose from 4 to 8 words in six minutes (`STT_ADMISSION_THRESHOLD_AB.md`).
  **Lowering both floors together has not been tested.**
- *Gain on the detector's input only* (the earlier high-pass and up to 24× "prior detector";
  recognition stayed raw) lowers both floors by up to 27.6 dB. It gave the biggest gains measured:
  - P4 quiet: 73 → 45 errors on 73 words;
  - P4 ordinary: 41 → 25;
  - constructed continuous P4: 88 → 65 on 146 words.
  It failed the strict gates: background 4 → 7 words in six minutes, 30 → 35 jobs, and more
  noise10 and AMI-SDM deletions. That is a product trade-off, not a dead end.
- *Raw detection at 1.25 × noise with onset confirmation* recovered quiet speech without
  amplification. Distant and noisy speech, and background retention, still failed
  (`STT_PRODUCT_HANDOFF.md`).

## 4. Ranked plan (needs the board, the bench Mac and the Audiodrive)

1. **Built 7 November 2026; not yet run on hardware.**
   - *What exists:* `sttraw on|off|status` (admin) with the `System_STTRawCapture` module, host
     replay via `test/host/stt_raw_replay.cpp`, and analysis in `tools/stt_raw/` (see its README).
     The segmenter settings moved to `stt/stt_continuous_config.h`, so the device and the replay
     share one definition.
   - *Verified:*
     - the ring passes under ASan/UBSan and TSan;
     - replay tests pass;
     - both `stt_continuous` variants and the command checks pass;
     - `p4-openclaw` builds with no warnings from the new code.
   - *First hardware check:* replay with no overrides must equal the session's `TL boundary` lines.

   The original plan for this step: **Continuous raw STT-input capture.** This enables everything else. It is a firmware tap in
   `sttCaptureWorker` that streams the exact HAL PCM the segmenter sees to the SD card for a whole
   session, like the standalone `pcm save` but continuous. Use it for one playback or live
   session, then replay every detector variant on the host through the real `stt_segmenter.h`
   (`test/host/stt_segmenter_harness.cpp` exists).
   - Every A/B arm gets the same audio exactly, with no device runs between arms.
   - It also answers "how much reference speech lies outside every segment" directly.
2. **Noise floor matrix**, using that capture. Conditions: port sealed; open in a quiet room; then
   Wi-Fi, LCD, camera and SD writes switched on one at a time. Also sealed with `amplify_num` 4.
   Report RMS, P25 and a Welch spectrum, and look for rail tones.
   - If the sealed floor is near −90 dBFS, the mic and electronics are fine. The floor is the room,
     and a better mic gains nothing.
   - If the sealed floor is well above −90 dBFS, the floor is electrical. Filter the mic's supply
     (series R or ferrite plus a cap at U9), or give it its own LDO. That is a direct SNR gain.
3. **Joint floor scaling**, the untested arm. Scale both floors together, keeping the relative
   `2 × noise` term:
   - (45, 16) → (32, 11), (24, 8), (16, 6);
   - each with and without N-frame onset confirmation.
   Use the same gates as before: no-caption share, substitutions, deletions and insertions,
   false captions on noise-only audio, and recognition jobs per minute. Include live two-person
   speech before adoption.
4. **Front-end DSP**: low priority.
   - High-pass filtering is already on at 35.5 Hz.
   - The recognizer applies CMVN per utterance, so plain gain does nothing.
   - The earlier recorder-chain A/B (73 → 65 errors) had per-phrase regressions.
   - ESP-SR AFE noise suppression cannot run beside STT today (`System_STT.cpp:2526-2528`).
5. **Hardware**, only after step 2.
   - *Mic VDD filter*: a few cents. Useful only if the floor turns out electrical.
   - *Higher-SNR mic*: Infineon IM73D122, 73 dB(A), −26 dBFS, 4 × 3 × 1.3 mm, 1.12 mA, about 9 dB
     less self-noise. Useless if the room sets the floor.
   - *Second mic on the same data line*, using the opposite L/R strap. The P4 supports up to 4 PDM
     RX lines on I2S0.
     - Two-mic beamforming gains only about 3 dB in diffuse room noise.
     - It needs stereo `DSR_16S` (check the clock-divider limit) and an AFE that can run beside
       STT.
   - *Talker distance and porting*: each halving of distance is about +6 dB on speech. Facing the
     talker was already better than facing away (quiet errors 45 vs 58).

## 5. Bench caveats

- **Replay is quieter than a person.** Playback at volume 70 gives about 60 dB SPL at 1 ft.
  - The "18 dB quieter" arm is about 42 dB SPL, close to the room floor (0–3 dB SNR). Even a
    perfect detector is near its limit there.
  - A soft live talker at 1 m is louder than that arm.
- **Replay changes the signal.** It smears speech beyond its level (§14 of the phase-4 plan).
  Confirm any detector change with live talkers.
- **Mapping is by arithmetic.** SPL figures in this note come from the datasheet sensitivity, not
  a calibrated meter. Measure once with a phone SPL meter at the mic.
