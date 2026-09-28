# P4 speech qualification and local STT investigation

Active worktree: `codex/jpeg-portable`, based on `611dd06` audio milestone.
Primary project checkout is untouched. S3 speech work is stopped; no speech
firmware/models were flashed to it. Objective: working P4 ESP-SR, followed by
fully local P4 STT research with no Raspberry Pi/UART requirement.

## Working speech milestone

ESP-SR 2.5.5 on IDF 5.5.5 is qualified on P4 with the shared onboard PDM HAL.
V8 app SHA `0cab35619787ffb0accff1324f71dea8bdf52d6e67376fb57bd7defb0ab86933`
(6,336,368 bytes); original stock model bundle SHA
`16f3ef0bd4d961da5811acded6a1f7c9b64dfa1ebd120705d7b6a3f7906e8a17`
(3,052,231 bytes). Partition table and unrelated LittleFS files preserved.

V8 private bench record `p4-bench-190702` passed: 25 vocabulary reloads (five
with camera active), acoustic authenticated system/status, disarmed rejection,
spoken voice/close, recorder restoration, camera JPEG capture, BLE scan/mesh
coexistence, and repeated start/stop. The final two stopped-state measurements
match exactly: PSRAM 32,835,420 bytes and internal RAM 246,659 bytes free.
SR task stack has 3,652 bytes free of 8,192 after spoken command transitions.
No HCI allocation failures, HCI timeouts, recovery or panics in this run.
Recognition tests use local synthesized speech via a speaker; human voices,
G2 microphone and active connected BLE accessories remain unqualified here.

The vendor dynamic-grammar leak is avoided by creating each MN6/7 instance
with the desired validated CSV through its public model-file descriptor.
The initial create uses a valid bootstrap; descriptors are restored immediately.
The failed v7 empty-default idea is explicitly rejected and retained in RESULTS.
P4 Hosted PSRAM preference provides internal-heap headroom. No binary patch or
opaque model offset is used. Vendor-internal OOM is not guaranteed recoverable.

## Fully local STT experiment

Official English QuartzNet5x5 checkpoint safely inspected and host-decoded on
two public clips. Int8 weight size looks plausible in PSRAM, but ~14.68 seconds
of future context and utterance normalization suit buffered dictation, not
low-latency captions. Current flash cannot hold both uncompressed model bundles
alongside all existing files; no user files were removed.

The standalone P4 operator probe compiled and ran successfully. Dominant
512-to-512 pointwise layer, T800: 88.245 ms native 1D single-core, 88.843 ms
2D single-core, 58.767 ms equivalent 2D multicore (medians of three runs after
warm-up). Outputs match byte-for-byte and 128 independent scalar references.
This is a synthetic kernel result, not whole-model inference or accuracy.
Next useful gate: real-weight calibrated export and depthwise/per-channel
checks, followed by full-model memory, speed and transcript comparison.
TRANSCRIPTION.md maps how to reuse dictation ownership and G2/OLED consumers.

## Completed device handoff

The standalone probe touched only the application slot. V8 was restored and
flash-verified again (`p4-speech-v8-restored-result.json`); boot capture had no
panic. P4-only `postcheck-191616.json` passed all 20 checks: mesh/peer keys and
channel, power, Bluetooth preferences, Wi-Fi/HTTP, camera settings, microphone
source/rate/gain, battery voltage and speech state. The first postcheck received
one incomplete battery JSON reply; a fresh check passed with complete responses.

Speech, camera and microphone are stopped; voice is disarmed; speech autostart
is false, model source2. Mac output is volume0/mutedtrue. No serial reader or
build process remains open. S3 firmware is unchanged. The final source manifest
verified all7,941 inputs after the operator probe build.

The work is saved locally on the current branch. Private weights, recordings,
credentials, serial logs and flash/filesystem backups remain ignored. No push
or PR was requested. Future STT work starts with the real-weight export gate
above; the P4 currently runs working command recognition, not free-text STT.
