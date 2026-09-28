# P4 ESP-SR 2.5.5 qualification

This experiment updates HardwareOne's existing voice-command consumer to the
ESP-SR 2.x API while retaining `HAL_Audio` for both onboard PDM and G2 input.
The implementation keeps a shared 16 kHz mono PCM interface. The P4 experiment
uses WakeNet9 “Hi, ESP”, English MultiNet7 and WebRTC VAD. Noise suppression
is disabled in the final AFE configuration; WakeNet AGC remains enabled.
The C6 provides radio transport; speech inference runs on the P4.

The current user-authorized scope is P4 only: first qualify local voice
commands, then investigate P4 speech-to-text models. S3 work is stopped. Its
installed firmware remains the previously qualified audio image; the stored
S3 speech profile and build evidence are unfinished research, not a release.

ESP-SR recognizes a configured command vocabulary. It is not a general-purpose
continuous transcription engine. Transcription feasibility is a separate task.

## Pinned build inputs

The preserved source baseline is audio milestone
`611dd06f36e105171dca286a01bf5395b805cb56`. `prepare.py` verifies
`../audio_portable/private/app-{p4,s3}` using `audio.check(current=False)`, then
creates independent copies. An exact-context patch carries production code,
CMake and SDK-default changes. Only the reviewed ESP-SR dependency is added to
the qualified deployment manifests. Existing package versions and hashes stay
unchanged; the new package set is locked per target:

| Package | Version |
| --- | --- |
| ESP-SR | 2.5.5 |
| ESP-DL | 3.3.12 |
| DL FFT | 0.7.0 |
| cJSON | 1.7.19~2 |
| esp_new_jpeg | 1.0.2 |

Keep ESP-IDF 5.5.5 and the previously qualified Bluetooth/JPEG SDK overlays.
ESP-SR's P4 revision-3 library selection requires IDF 5.5.3 or newer; an IDF 6
upgrade is unnecessary. The official registry package corresponds to source
commit `84a5d9acf5e4266ec576596a09b6fd236a434b12`.

`HW1_SR_MODELS_IN_FILESYSTEM=1` explicitly retains the existing application and
LittleFS partition layout even with speech enabled. Models are separate files;
changing this option is a storage migration and must not happen implicitly.
Neither profile disables the qualified Bluetooth, camera, mesh or other
features to make space for speech. The general production manifest also pins
ESP-DSP 1.8.0 explicitly for the existing onboard LLM kernels; ESP-SR no longer
brings that dependency transitively on P4/S3. These speech profiles keep the
onboard LLM disabled as in their audio baseline.
The S3 v2 image compiled and linked, but its `0x636420`-byte binary exceeded
the preserved `0x5b5000`-byte application partition by 529,440 bytes. An `-Os`
size-optimization attempt was prepared and its build stopped at the user’s
request. Its final size and runtime behavior are unverified. No S3 firmware,
filesystem or partition table was flashed, and the general S3/ESP32 root
lockfiles were left unchanged.

## Build copies and model bundle

With the pinned IDF environment activated, and production edits stable:

```sh
python3 -B experiments/speech_portable/prepare.py --capture
python3 -B experiments/speech_portable/prepare.py --target p4
bash experiments/speech_portable/build.sh p4
python3 -B experiments/speech_portable/pack_models.py --target p4
```

Use `build.sh p4 reconfigure` for configuration without firmware compilation.
Use `prepare.py --target p4 --check` to verify a copy. After intentional source
changes, stop builds, capture again and explicitly `--refresh` the P4 copy.
Refresh verifies the old source tree first. Never edit a qualified audio copy
or refresh a speech copy during a build. The P4-only
`sdkconfig.p4.speech.defaults` prefers Hosted transport buffers in PSRAM, with
internal DMA RAM as fallback, to reduce internal-heap pressure during speech
model replacement. The inherited Hosted allocator remains unchanged: IDF
5.5.5's `esp_heap_adjust_alignment_to_hw()` already rounds and cache-aligns
`SPIRAM|DMA` allocations, then removes the generic DMA capability before heap
selection. The P4 SDMMC path performs cache sync and bounces short/unaligned
transfers. This option does not change the S3 profile.

For an existing P4 generated sdkconfig, after refreshing the stopped copy,
explicitly run `prepare.py --target p4 --apply-p4-config` before rebuilding.
It accepts only the known disabled setting, backs up the complete old config
under its SHA-256, rechecks its bytes, then changes that single setting. A
fresh build uses the new defaults; `build.sh` rejects a stale existing config.

The lock check permits Component Manager to refresh its aggregate manifest
hash; package versions, content hashes and dependency constraints must still
match the checked-in target lock. After a completed configuration/build, use
`prepare.py --target p4 --accept-lock` to seal refreshed lock metadata
into the source manifest. Missing cached packages are fetched by Component
Manager from the pinned lock when configuring. Build outputs, raw serial logs, recordings,
credentials and flash/filesystem backups belong in ignored `private/`.

`pack_models.py` verifies the package identity and configured model choices,
then writes `private/models-<target>/srmodels.bin` with deterministic sorted
model/file ordering. Its accompanying manifest records source-file hashes,
package version/hash, SDK configuration hash and final bundle hash. The layout
matches Espressif's packer. It uses only assets from this exact package,
including its current FST files; older generated FST fixtures are not release
model bundles. Every packed file payload, including the stock
`fst/commands_en.txt` demo grammar, is byte-identical to the pinned package.
The packer checks the complete expected bundle SHA-256
`16f3ef0bd4d961da5811acded6a1f7c9b64dfa1ebd120705d7b6a3f7906e8a17`.
Its schema-2 manifest records source hashes, the packer hash and an empty
`packed_transforms` map. A prior empty-grammar experiment was rejected by the
P4 hardware test: the vendor create wrapper crashes on an empty command list.
No grammar payload suppression is part of the restored bundle. Runtime
command-table lifecycle work uses the public model descriptor instead; see
[RESULTS.md](RESULTS.md) for the qualified firmware state.

The runtime loads `/ESP-SR Models/srmodels.bin` when `srmodelsource` is 2, or
`/sd/ESP-SR Models/srmodels.bin` when it is 1. It validates the packed structure
before passing a retained PSRAM buffer to ESP-SR. Model-source choice remains
explicit; choosing file-backed partitions does not silently change saved
settings. Model memory is released only after speech workers and inference
objects have stopped.

P4's existing LittleFS has room for the roughly 3 MB bundle. The S3's current
2,338,816-byte filesystem does not, and no S3 card is available. A future S3
effort would need both application-size and model-storage qualification; SD
is supported by the shared file loader but has not been tested here. S3 model
loading, missing-model behavior and recognition were not tested on hardware.

## Using the installed P4 experiment

After signing in to HardwareOne's console, run `opensr`. Say “Hi ESP”, pause
about two seconds, say “system”, pause again, then “status”. `srstatus` reports
recognition state; `closesr` stops it. Starting from an authenticated console
arms voice with that user's permissions. Boot auto-start remains off.

See [RESULTS.md](RESULTS.md) for hardware evidence and recognition limits, and
[TRANSCRIPTION.md](TRANSCRIPTION.md) for fully local P4 STT investigation.
ESP-SR commands and general transcription are separate recognition backends.

Custom MultiNet command IDs are limited to 1–1023 because the vendor allocates
lookup arrays by highest ID; HardwareOne reserves 990–992 for global commands.
MN6/7 command-file serialization also rejects commas/newlines and records that
would exceed the vendor’s 128-byte parser buffer.

## Qualification boundary

A successful build does not prove recognition. Physical checks should cover
model initialization and memory use, wake and allowed command detection,
start/stop/restart, exclusive microphone ownership, camera/mesh coexistence,
authentication boundaries and restoration of unrelated saved state. Connected
G2 capture needs its own test. Compare partition tables and preserve existing
user files before any installation; the build/pack scripts never flash.

## Primary references

- [ESP-SR 2.5.5 registry and supported targets](https://components.espressif.com/components/espressif/esp-sr/versions/2.5.5/readme).
- [Official 2.5.5 dependencies](https://components.espressif.com/components/espressif/esp-sr/versions/2.5.5/dependencies?language=en).
- [AFE 1.x to 2.x migration](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/migration_guide.html): replace `AFE_CONFIG_DEFAULT()` and static AFE handles with `afe_config_init("M", ...)` and `esp_afe_handle_from_config()`.
- [Exact-release model loader](https://github.com/espressif/esp-sr/blob/84a5d9acf5e4266ec576596a09b6fd236a434b12/src/model_path.c): `srmodel_load` retains pointers into its supplied packed buffer.
- [P4 resource benchmarks](https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/benchmark/README.html) and [S3 resource benchmarks](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/benchmark/README.html): MultiNet7 lists approximately 2.9 MB PSRAM before AFE and application costs; file-backed model buffers consume additional memory.
