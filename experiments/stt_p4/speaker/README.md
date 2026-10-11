# Speaker-label model (speaker.spk)

The P4 labels each continuous-transcription phrase with a session-local speaker
("[Person N]" in the saved transcript) when `/sd/STT Models/speaker.spk` is on
the microSD card and the firmware has `ENABLE_STT_SPEAKER_LABELS` (the P4X-EYE
profile does). Without the file nothing changes. Device side:
`components/hardwareone/stt/speaker_frontend.*`, `speaker_model.*`,
`speaker_cluster.h` and `sttSpeaker*` in `System_STTLocal.cpp`; the file format
is described in `speaker_model.h`.

The current model is the clova Fast ResNet-34 (`ResNetSE34L`, 1.4 M weights,
[clovaai/voxceleb_trainer](https://github.com/clovaai/voxceleb_trainer), MIT
code; pretrained `baseline_lite_ap.model`), exported with esp-ppq as 8-bit
weights / 16-bit activations (8-bit activations lose 4-6 points of labelling
accuracy). Any embedding model that its file describes can replace it.

- `proto.py` - the model in PyTorch with the device's front end, synthetic
  conversations from the speech stores, online labelling and a threshold sweep.
- `export_speaker.py` - quantizes the trunk, chooses the threshold on the dev
  stores (simulated glasses channel) with the device's own labelling rules, and
  writes `speaker.spk`, `fixture.bin` (the diagnostics build's parity check)
  and `report.json`.

Environment: `STT_ROOT` (default `/Volumes/Audiodrive/stt`: speech stores and
outputs), `HW1_SPEAKER_CLOVA` (default `$STT_ROOT/hw1-work/speaker/clova`: the
voxceleb_trainer checkout with `baseline_lite_ap.model`, kept out of git). Run
with the QuartzNet export venv (`../private/export-venv/bin/python`, esp-ppq).

To install: copy `speaker.spk` to `STT Models/` on the card. Its SHA-256
trailer is checked at load; a different model needs no firmware change.
