# P4 speech qualification — 2026-09-28

## Scope and installation

ESP32-P4 revision 3.2, 16 MiB flash and 32 MiB PSRAM, using the onboard PDM
microphone through the shared audio HAL. The attached CSI camera has no display.
ESP-IDF stays at 5.5.5; ESP-SR is pinned to 2.5.5. C6 firmware is unchanged.
S3 speech qualification was stopped at the user's request. No S3 speech image
or model bundle was flashed.

A verified full-flash backup was taken first. The P4 partition table and
existing LittleFS files were preserved; only the model file was added and
`srmodelsource` was explicitly set to 2. Models are WN9 hiESP and English MN7,
packed from the exact pinned package: 3,052,231 bytes, SHA-256
`16f3ef0bd4d961da5811acded6a1f7c9b64dfa1ebd120705d7b6a3f7906e8a17`.
The path is `/ESP-SR Models/srmodels.bin`.

## Physical evidence before the final vocabulary-lifetime fix

Private records `p4-bench-175404.json`, `p4-bench-180549.json` and
`p4-bench-180844.json` contain redacted command replies, snapshots and timestamps.
Local synthesized English fixtures were played through the Mac speaker into
the P4 microphone. These are acoustic tests, not injected PCM. Mac output
volume/mute were restored after each playback; the Mac microphone was not used.
No human-voice result was available at this stage.

| Check | Observation |
| --- | --- |
| Missing models | Selecting the absent model partition returns an actionable error and releases microphone/resources. |
| Model initialization | The real file bundle loads, AFE/MultiNet start, and continuous PCM input reaches approximately 16 kHz. |
| Full authenticated command | “Hi ESP” → “system” → “status” completes through the normal command pipeline, twice in v3 sessions. |
| Spoken self-stop | “Hi ESP” → “voice” → “close” completes and stops/disarms SR without deadlock, three times. |
| Authentication | The recognizer correctly hears system/status while disarmed; dispatch refuses it and the executed-command count stays unchanged. |
| No-wake control | system/status/unrelated speech without the wake phrase produces no command. |
| Unrelated speech after wake | Wake followed by “purple bananas dance quietly” times out without a command. |
| Exclusive microphone ownership | Recorder start is refused while SR owns the HAL. |
| Recorder restoration | Starting SR with the recorder already open suspends it; stopping SR restores the PDM recorder at saved 16 kHz/gain70. |
| Lifecycle | Three no-speech start/stop cycles leave internal free heap at 154 KiB and PSRAM within 1 KiB of the 32,089 KiB baseline. |
| Camera coexistence | Camera starts during SR and returns a 7,102-byte JPEG at 240×240. Both remain healthy; camera is stopped afterwards. |
| Mesh coexistence | Existing encrypted peer broadcasts continue receiving acknowledgements during speech and camera checks. This is not a mesh throughput benchmark. |

With speech alone, roughly 25 MiB PSRAM and 93 KiB internal heap remained.
With camera also started, roughly 19.8 MiB PSRAM and 72 KiB internal heap
remained. These are snapshot free-memory values, not peak allocator traces.
Speech/model objects release about 6.2 MiB on stop.

## Recognition limits and tuning

The old high-pass/pre-emphasis chain harmed the wake phrase in a paired test:
zero of four attempts with it on, then three of three with it off at the same
speaker volume and mic gain. The SR-only default is now off; recorder behavior
is unchanged. AFE noise suppression is off per the ASR recommendation.

The voice/close fixture initially missed the category threshold. It later
passed with dynamic gain both on and off; the existing dynamic-gain default
therefore remains on. Thresholds were not lowered to make the tests pass.
These few synthetic-voice trials do not establish room-wide reliability,
accent coverage, false-accept rate or microphone placement tolerance. G2
microphone input was not physically retested in this milestone.

## Issue found by repeated vocabulary changes

Plain start/stop is stable, but v3 lost approximately 8 KiB for each command
vocabulary replacement: five `srcmdssync` calls lost 39 KiB, still missing after
stop. Successful two-stage utterances lost approximately 16 KiB each, including
an unarmed utterance, isolating the issue from command execution.

Inspection of the pinned P4 MultiNet7 library found two arrays overwritten by
its FST rebuild without releasing their previous allocations. Their size is
2 × maximum command ID × 4 bytes; global IDs up to 992 give 7,936 bytes per
replacement, matching the hardware observation. The application workaround
uses supported model lifecycle APIs, avoiding binary patches or opaque offsets.
The bounded v8 memory and latency measurements are recorded below.

### Intermediate v6 findings

The public-API recreation preserved spoken system/status and voice/close.
The task's corrected stack measurement was 3,732 free bytes of an 8,192-byte
allocation. Measured category-to-next-prompt times were about 608 ms for system
and 322 ms for voice; this adds latency compared with in-place updates.

Five reloads initially reduced the observed loss from 39 KiB to about 4 KiB.
Further inspection found that model creation itself publishes the bundled
demo grammar, so publishing a different table afterward still leaks once per
model lifetime.

V6 also exposed a separate interaction twice: an internal-DMA HCI allocation
failed during recognizer recreation, followed by a Bluetooth command timeout
and synchronous recovery blocking serial ingress. The v7 P4-only defaults
enable the existing `ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` option. The qualified
Hosted allocator and IDF 5.5.5 P4 SDMMC alignment/cache path already support it;
no additional allocator or SDK patch was introduced. Cold-boot internal free heap improved from roughly 157 KiB to 244 KiB.
Speech coexistence validation was prevented by the separate v7 failure below.

Manual custom command lists are not qualified by the route tests. The pinned
vendor add API also has a small phoneme-allocation leak on duplicate/invalid
manual additions; normal HardwareOne route loads deduplicate before adding.

### Rejected v7 empty-default experiment

Replacing only `fst/commands_en.txt` with one NUL byte was **not safe**. Hardware
record `p4-bench-184913` shows `opensr` panicking with a load access fault at
`0x401ebc94`, inside the pinned vendor `model_init` wrapper. Its internal create
rejects zero commands and returns NULL; the wrapper dereferences NULL+0xb0.
The prior host mock and partial disassembly analysis did not model that wrapper
failure. This result invalidates the empty-default approach. The board rebooted
with speech autostart disabled, and the empty bundle was retained privately as
failed-test evidence.

The v8 correction restores the byte-identical stock bundle and provides each
desired grammar through the public model-file descriptor for the duration of
model creation. Creation publishes once, without a later FST replacement. A
valid bootstrap grammar is used for initial creation, and the descriptor is
restored before the temporary CSV is freed. Host regressions cover CSV bounds,
IDs, phonemes, thresholds, descriptor restoration and rejection before teardown.
No vendor binary patch or opaque model offset is used. A vendor-internal
allocation failure can still panic in its create wrapper; application-level
NULL handling cannot guarantee recovery from that internal fault.


## V8 qualification

Private record `p4-bench-190702.json` and its serial log cover approximately
3 minutes 47 seconds, from 19:07:02 to 19:10:49 UTC. The installed application
is 6,336,368 bytes, SHA-256
`0cab35619787ffb0accff1324f71dea8bdf52d6e67376fb57bd7defb0ab86933`.
The stock model bundle above was restored and verified. The partition table
and other LittleFS files were preserved. The existing P4 Hosted PSRAM preference
remained enabled; C6 firmware was unchanged.

### Repeated vocabulary changes and lifecycle

All 25 `srcmdssync` commands completed: 20 with speech alone, then five with
speech and camera together. Free PSRAM from heap diagnostics was:

| Snapshot | Free PSRAM (bytes) | Status free PSRAM (KiB) |
| --- | ---: | ---: |
| Cold boot | 32,841,236 | 32,073 |
| Speech started; before reloads | 26,328,452 | 25,711 |
| After 10 reloads | 26,327,176 | 25,710 |
| After 20 reloads | 26,327,736 | 25,710 |
| Camera and speech started | 20,672,464 | 20,187 |
| After five additional reloads with camera | 20,672,412 | 20,187 |
| Stopped after camera coexistence | 32,835,424 | 32,065 |
| First subsequent start/stop | 32,835,420 | 32,065 |
| Second subsequent start/stop | 32,835,420 | 32,065 |

The first ten reloads changed free PSRAM by −1,276 bytes; the next ten recovered
560 bytes. The five camera-coexistence reloads changed it by −52 bytes. The
final two start/stop cycles returned exactly equal diagnostic readings for
both PSRAM (32,835,420 bytes) and internal heap (246,659 bytes).

These results remove the previous approximately 8 KiB-per-reload slope in this
bounded run. They do not prove absolute zero leaks or long-duration reliability:
BLE scanning/reconnection, logging and mesh traffic continued, and no allocator
trace isolated their allocations. Status and heap diagnostics are separate
commands, so their values can differ as background work runs. Minimum recorded
internal free heap during the complete run was 105,508 bytes, including the
camera/reload phase; internal free heap in camera-running snapshots was about
159 KiB.

### Commands, microphone and camera

The same acoustic synthetic-voice method was used; no human voice was tested.

| Check | V8 result |
| --- | --- |
| Armed system/status | “Hi ESP” → “system” → “status” executed through the authenticated command path; command count increased from 0 to 1. |
| Disarmed system/status | Recognition succeeded, dispatch refused it, and command count stayed at 1. |
| Spoken self-stop | After re-arming, “Hi ESP” → “voice” → “close” executed `closesr`; command count became 2 and SR stopped/disarmed without deadlock. |
| Speech task stack | After the spoken status command, 3,652 bytes remained at the high-watermark of the 8,192-byte allocation; estimated peak usage was 4,540 bytes. |
| Grammar transition latency | Category selection to the next prompt was about 239–243 ms for system and 221 ms for voice in these trials. |
| Microphone restoration | `openmic` → `opensr` → `closesr` restored the PDM microphone at saved 16 kHz, 16-bit mono and gain 70; subsequent `closemic` succeeded. |
| Camera coexistence | OV2710 CSI camera captured a 7,244-byte JPEG while SR ran, using hardware JPEG at 240×240 from a 1280×720 center crop. Both stayed healthy through five further vocabulary reloads. |
| Cleanup and restart | Camera and SR stopped; two further SR start/stop cycles completed. The final bench snapshot has SR stopped/disarmed, no AFE/MultiNet handles or last error, and microphone/camera off. |

### Background transport observations and limits

Across the complete v8 log there were **zero** Hosted HCI allocation failures,
HCI `command_timed_out` messages, host-stack recovery requests or panics.
There were 43 recorded mesh broadcast acknowledgements, all 1/1 peers (100%),
with completion times of 73–599 ms. This supports coexistence in this run;
it is not a comprehensive mesh throughput or packet-loss benchmark. Existing
G2/R1 reconnect scanning remained enabled, but their live connections were not
requalified here.

The bench completed with no test errors. Its log still contains 43 background
`rpc_wifi_sta_get_ap_info` failures while no access point was connected, four
AFE empty-ringbuffer messages during shutdown and 12 loop-stall warnings,
mainly around synchronous startup/reload work. These are not a clean-log claim.

This section records the v8 bench, before the separate isolated STT probe and
subsequent v8 restore/postcheck.


## Restored application and saved-settings check

After the isolated STT operator experiment, the exact v8 application above was
restored with an app-only write and flash verification. The captured boot had
no panic. P4-only record `postcheck-191616.json` passed all 20 preservation
checks, including encrypted mesh peers/channel, power settings, Bluetooth
preferences, HTTP/Wi-Fi state, camera configuration, microphone source and
16 kHz/gain70, battery voltage, and speech source/autostart/stopped state.
An earlier postcheck was interrupted by one incomplete battery-status JSON
reply; a fresh check returned valid complete data and passed without a retry.

The board is left with speech, camera and microphone stopped and voice disarmed;
speech autostart remains false. The Mac's output volume/mute are restored to
0/true. No S3 speech firmware or models were installed. The final build-input
check verified all 7,941 preserved sources after the operator build. Model
weights, raw logs, credentials and backups remain in ignored private storage.
