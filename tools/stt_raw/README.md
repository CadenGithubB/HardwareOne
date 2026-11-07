# STT raw-input recordings (`sttraw`)

`sttraw on` makes the next continuous `stt start` session save exactly what the speech
segmenter receives. That is the raw HAL PCM, plus any pause silence the broker feeds, written
to `/sd/STT Raw/raw_<id>.pcm` as s16le mono at 16 kHz. A `raw_<id>.json` sidecar records the
sample counts and any gaps. One recording can then be replayed on a host through the production
segmenter at any detector setting, so every A/B arm sees the same audio exactly.

- **Admin only.** The command needs an administrator, and the audio is room audio stored
  unencrypted on the card.
- **Not persistent.** Arming lasts until `sttraw off` or a reboot. `sttraw off` also stops an
  active recording.
- **Continuous sessions only.** Bounded `stt record` sessions are not recorded.
- **Memory.** Recording uses a 256 KiB PSRAM ring and a low-priority writer task (`stt_rawsave`,
  core 0) only while active.
- **STT is never affected.** A full ring drops whole blocks and lists them under `gaps`. Any
  failure appears in `sttraw status`, and the session carries on.
- **Serial log.** It shows `TL raw_start path=...` and `TL raw_end ...`. The session's
  `TL boundary seg=N start=S end=E` lines give the device's own phrase boundaries in the same
  sample units.

## Replay

Build the CLI with the host tests (`components/hardwareone/test/host`, target
`stt_raw_replay`), or directly:

```bash
clang++ -std=c++17 -O2 -Icomponents/hardwareone components/hardwareone/test/host/stt_raw_replay.cpp -o stt_raw_replay
```

```bash
./stt_raw_replay raw_1a2b3c4d.pcm --sidecar raw_1a2b3c4d.json > base.json
./stt_raw_replay raw_1a2b3c4d.pcm --speech-floor 24 --silence-floor 8 --cut-dir cuts-24 > f24.json
```

- **No overrides:** the segments must equal the device's `TL boundary` lines. Check that first;
  it proves the replay is exact.
- **Settings:** the defaults come from `stt/stt_continuous_config.h`, the same function the
  firmware calls.
- **`--cut-dir`:** writes each phrase's PCM (`seg_<n>.pcm`) for a host decoder. Note that the
  device decodes a phrase only up to its voiced end plus 500 ms (`voiced_end` in the JSON).
- **`--trace`:** writes per-frame `rms, noise, threshold, open`.

## Analysis

```bash
python3 tools/stt_raw/analyze_raw.py raw.pcm --from 2 --to 62 --spectrum
python3 tools/stt_raw/analyze_raw.py raw.pcm --replay base.json --speech intervals.csv
```

- **Levels** are frame-RMS percentiles in the segmenter's units and in dBFS. Use them for the
  noise-floor matrix.
- **`--spectrum`** needs numpy. It lists narrow tones, which point to electrical pickup.
- **Coverage** reports the share of reference speech outside every segment, and the intervals
  with no segment at all. The reference intervals come from a playlist or cue schedule mapped
  into file seconds.

## Bench protocol

1. Run `sttraw on`, then `stt start`. Wait for Listening before any speech or playback; the first
   500 ms is calibration.
2. Noise-floor matrix: record 60 s per condition, interleaved, in the same position.
   - Mic port sealed (putty or tape over U9's port).
   - Open, quiet room.
   - Open, with Wi-Fi traffic, then the LCD on, then the camera streaming, then SD writes
     (another recording running).
   - Sealed, built with `amplify_num = 4`: a separate build, to check for bit loss.
3. Detector A/B: one playback or live session recorded once, then every arm replayed. Score
   captions per arm with one host decoder, and report:
   - substitutions, deletions and insertions;
   - clips with no caption;
   - false captions on noise-only stretches;
   - recognition jobs per minute.
4. Keep recordings off the internal disk (`/Volumes/Audiodrive`) and out of Git.
