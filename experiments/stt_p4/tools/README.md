# STT on P4: bench and device tools

Scripts that drove the hardware validation recorded in this experiment's
`*-validation.json` files and result notes. They talk to boards over USB
serial and the HTTP API; none of them flashes firmware.

| Group | Scripts | Used for |
|---|---|---|
| Benches | `bench.py`, `continuous_bench.py`, `transcript_bench.py`, `transcript_oneshot.py`, `transcription_ui_bench.py`, `transcription_ui_preflight.py`, `transcription_ui_usb.py`, `transcription_network_state.py` | `validation.json`, `continuous-validation.json`, `transcript-validation.json`, `transcription-ui-validation.json` |
| Probes and installs | `run_app.py`, `run_probe.py`, `run_cache_probe.py`, `install_model.py` | probe images, model installation, `cache-validation.json` |
| G2 / S3 helpers | `g2_user_connect.py`, `s3_keep_web.py` | glasses connection and S3 web keep-alive during runs |
| `device/` | `p4_mic_test.py`, `p4_upload.py`, `p4_cli.py`, `p4_steps.py`, `p4_fts*.py` | speaker-to-PDM-mic scoring, chunked `filewrite` upload, scripted serial CLI and setup screens |
| `deploy15-analysis/` | `macs.py`, `liveness.py`, `timing.py`, `parity15.py`, `run_eval.sh` | QuartzNet 15x5 deploy measurements in `PROFILE_15x5.md` |

## Credentials and board identity

No credentials are stored in the repository. The bench scripts read a JSON
file with `username`, `password` (and, where used, `ap_ssid` / `ap_password`)
from the path in `HW1_CREDENTIALS`, defaulting to the ignored
`experiments/p4_ble_roles/private/credentials.json`. The `device/` scripts read
`P4_USER` / `P4_PASS` from the environment. The benches check they are
talking to the right board by its ESP-NOW MAC: set `HW1_P4_MAC` to the address
`espnowstatus json` reports. The default is a placeholder (`02:48:57:31:..`)
that matches no real board.

Run them from the repository root with the Python environment described in
`../README.md`.
