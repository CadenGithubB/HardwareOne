# Shared microphone qualification

This experiment builds HardwareOne's existing `HAL_Audio` microphone path for
both attached boards, retaining the qualified camera, JPEG, battery and radio
integration. The local PDM microphone and G2 glasses remain sources behind the
same exclusive capture API. Consumers continue receiving mono signed 16-bit
PCM; the shared default is 16 kHz. Board wiring belongs in board configuration.

| Board | Local microphone | PDM clock | PDM data | Power |
| --- | --- | --- | --- | --- |
| XIAO ESP32-S3 Sense | Expansion-board digital PDM microphone | GPIO42 | GPIO41 | Board supply |
| ESP32-P4X-EYE | U9 MSM261D digital PDM microphone | GPIO22 | GPIO21 | GPIO12 high enables shared `3V3_PERI` |

P4 uses the same I2S0 PDM-to-PCM driver as S3. It needs no I2C codec and no
additional managed component. The P4X reference schematic pulls the microphone's
L/R input to ground, selecting the left slot. Its shared peripheral power rail
also serves the camera and LCD, so microphone cleanup must not switch it off.
The C6 is not involved in local microphone capture.

## Reproducible build copies

The production diff is captured against camera milestone
`05614eb5900e461633ad951f7358b522894e313b`. The input applications are preserved
`../camera_portable/private/app-p4` and `app-s3`. Preparation calls
`camera.check(..., current=False)` to verify those sealed copies without
requiring the active production tree to remain at the camera milestone.

`prepare.py` copies the verified source files and managed components into
separate ignored `private/app-p4` and `private/app-s3` directories. It applies
only an exact-context production patch and records a SHA-256 manifest of the
resulting overlay, scripts, profiles, inherited SDK defaults and radio sources.
The earlier camera snapshots, dependency locks and installed SDK remain intact.
Component dependency changes are rejected for this milestone. The P4 copy keeps
`dependencies.lock.esp32p4`; the S3 copy keeps `dependencies.lock`.

The feature profiles are flattened from the camera profiles because the
existing CMake feature parser reads literal definitions. Both profiles turn on
`ENABLE_MICROPHONE_SENSOR=1`. Camera, encrypted mesh and Bluetooth/G2 support
remain enabled; ESP-SR remains disabled for board-only qualification. P4 SD is
disabled. The S3 Sense profile declares its physical expansion-board SD pins,
but no card is inserted or qualified; recordings use the existing internal-flash
fallback.
P4 retains the display/encoder profile, which can operate without an attached
LCD. S3 retains its headless profile.

Run the following **only after the production audio edits are frozen**, from
the repository root with the pinned ESP-IDF 5.5.5 environment activated:

```sh
python3 -B experiments/audio_portable/prepare.py --capture
python3 -B experiments/audio_portable/prepare.py --target p4
python3 -B experiments/audio_portable/prepare.py --target s3
bash experiments/audio_portable/build.sh p4
bash experiments/audio_portable/build.sh s3
```

Builds inherit the qualified camera SDK defaults, the verified Bluetooth SDK
overlay, and the P4 JPEG SDK overlay. In particular, S3 camera PSRAM DMA stays
disabled as qualified previously. No IDF 6 upgrade is required. No command above
opens serial ports or flashes devices.

Check a prepared copy before using its output:

```sh
python3 -B experiments/audio_portable/prepare.py --target p4 --check
python3 -B experiments/audio_portable/prepare.py --target s3 --check
```

After an intentional production edit, first stop all builds, capture again,
then explicitly use `--refresh` for each prepared target. Refresh verifies the
old sealed copy before changing its overlay. Never refresh during a build.
Build images and logs belong in `private/`; recordings, credentials, flash
backups and raw console logs must also remain ignored and private.

## Qualification scope

Build preparation alone is not evidence of working audio. Physical testing
should cover each local source, actual WAV format/sample data, repeated
start/stop, unavailable G2 fallback, and camera plus recording together.
Compare audio rates/settings before and after testing, preserve account and
mesh data, and restore test settings. G2 uses the existing HAL source; live G2
capture requires connected glasses and must be reported separately from local
microphone qualification. Only the hardware coordinator owns serial ports.

## Hardware and driver references

- [Official ESP32-P4X-EYE guide and reference design](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32p4/esp32-p4x-eye/user_guide.html).
- Local P4X-EYE-MB V2.4 schematic, inspected sheets 2-5:
  primary checkout `work/p4x-enclosure/reference/ESP32-P4X-EYE/01_Schematic/SCH_ESP32-P4X-EYE-MB_V2.4_20260202.pdf`.
  U9 is powered from `3V3_PERI`; R246 is its 10 kohm left-slot strap.
  GPIO22/21 connect through R23/R255 respectively; GPIO12 controls the rail.
- [Espressif factory BSP PDM initialization](https://github.com/espressif/esp-dev-kits/blob/master/examples/esp32-p4-eye/examples/common_components/esp32_p4_eye/esp32_p4_eye.c#L616)
  uses I2S0, mono 16-bit PDM and a 16 kHz default without an I2C codec interface.
- [ESP-IDF 5.5.5 P4 I2S driver](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32p4/api-reference/peripherals/i2s.html#pdm-rx-mode-in-pcm-format-with-pdm-to-pcm-converter)
  supports hardware PDM-to-PCM conversion. Default 8S downsampling produces a
  1.024 MHz PDM clock for 16 kHz PCM; `i2s_channel_read` takes milliseconds
  per DMA-buffer wait. Partial bytes returned on timeout remain valid PCM.
- The P4 BOM identifies U9 as [MSM261DHP006](https://www.denovocn.com/sites/default/files/MSM261DHP006.pdf);
  the XIAO schematic specifies [MSM261D3526H1CPM](https://files.seeedstudio.com/wiki/XIAO-BLE/mic-MSM261D3526H1CPM-ENG.pdf).
  Both specify low-power clocks through 900 kHz and standard operation from
  1.1 MHz. Their board definitions supply those bounds to the same HAL logic:
  when the default 64× clock falls between those modes, use 128× decimation.
  Thus 8/16/48 kHz PCM use 0.512/2.048/3.072 MHz PDM clocks respectively.
  PCM sample rates and source IDs remain unchanged.
- [Qualified shared camera milestone](../camera_portable/RESULTS.md).

## Tests

```sh
python3 components/hardwareone/test/host/test_audio_hal_pdm.py --sanitize
python3 components/hardwareone/test/host/test_live_audio.py --sanitize
python3 experiments/audio_portable/test_wav_validation.py
```

The explicit `test_hardware.py` requires an existing private credentials JSON,
verified USB port and logical radio MAC. Run one board at a time. For example:

```sh
python3 experiments/audio_portable/test_hardware.py --board p4 \
  --port /dev/cu.usbmodem2201 --mac FC:01:2C:E0:B9:A8 \
  --credentials /absolute/private/credentials.json --tone-player /usr/bin/afplay \
  --tone-volume 40
```

On macOS, `--tone-player` plays a quiet 997 Hz reference from the workstation
speaker while the board records; it does not use the workstation microphone.
Optional `--tone-volume 1..50` temporarily unmutes macOS output at that volume
only during a tone and restores the exact prior volume/mute state afterward.
Without `--tone-player`, qualification checks dynamic PCM but makes no acoustic
frequency claim. The harness checks mono 16-bit WAV headers, sample duration,
clipping and (when enabled) the reference frequency at 8/16/48 kHz; exercises
restart cycles and camera coexistence while checking initialized/paired mesh
status (no mesh test packet is sent); removes only its correlated test
recordings; and restores source/rate/runtime states. No SD card is required:
these profiles use existing internal-flash recording fallback.

WAV downloads use validated, correlated 512-byte JSON chunks. A bounded retry
can only reread the same range of the same owned test recording. This read-only
path avoids a second `whoami` reply per chunk; state-changing commands retain
the console's FIFO completion barriers. The offline validator suite covers
truncated/stale replies, malformed payloads, retry limits and fatal guards.

For a deliberately scoped rerun, `--rates 48000` selects just that rate before
restart/coexistence checks; `--skip-coexist` runs the selected rates and restart
cycles only; `--coexist-only` runs camera/source checks without the rate/restart
phase. The two scope flags are mutually exclusive. Each invocation writes a
separate private result: a scoped pass proves only its recorded checks. After
a fatal console interruption, recover the board and verify settings before
starting a scoped rerun; the earlier run's cleanup is not assumed successful.

Before any flash, verify a full backup and physical chip identity, compare the
new partition table with the backup, and flash only the application partition.
Keep exact image hashes, backups and all captured audio in ignored `private/`.
See `RESULTS.md` for actual hardware evidence and remaining limitations.
