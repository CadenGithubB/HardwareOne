# Unified Microphone Source Layer (PDM + G2) — Verified Plan

**Status:** design verified (16-agent adversarial fan-out, 2026-07-24). Implementation not started.
**Goal:** make the Even G2 glasses mic a first-class, switchable microphone **source** alongside the onboard PDM mic, surfaced on the Sensors mic page (level + record — full mic, not SR-only), with **one** device-wide source shared by the sensor mic *and* ESP-SR voice.

## Two hard requirements (user decisions)
1. **One device-wide source**, shared by the sensor-page mic (VU/level/capture/record) **and** ESP-SR voice — no split-brain. Modeled on how `HAL_Input` unifies gamepad + ANO encoder.
2. **No default to G2 (or any absent source).** Each source is available **only when actually connected at runtime**: PDM only on a board that physically has it; G2 only when the **left** temple is connected. Selector lists only connected sources; if none, the mic feature is unavailable.

## Key reframe
The source-agnostic layer **already exists**: `HAL_Audio` (`audioSetSource`/`audioCaptureStart`/`audioReadPcm`, canonical 16 kHz mono, exclusive single-owner) and `System_Microphone` already pulls level/capture/record through `audioReadPcm`. The blocker is **gating**: the whole HAL is `#if ENABLE_MICROPHONE_SENSOR`, which is `0` on the primary FeatherS3 — so it vanishes on the exact board where G2 is the only mic. This is ~90% a de-gate + wiring job, not new plumbing.

## What the adversarial pass corrected (do NOT skip these)
- **Gate formula.** `ENABLE_MICROPHONE_SENSOR || ENABLE_G2_GLASSES` is WRONG — `ENABLE_G2_GLASSES` is `#define 1` and never `#undef`'d (the BT dependency is applied only at compound use-sites). Use `#define ENABLE_MICROPHONE (ENABLE_MICROPHONE_SENSOR || (ENABLE_BLUETOOTH && ENABLE_G2_GLASSES))`. Must be a plain literal (CMake greps `#define NAME <int>`, can't evaluate `(A||B)`).
- **`System_SensorStubs.{h,cpp}` MUST be flipped in lockstep** to `#if !ENABLE_MICROPHONE`. They define the same 9 globals + `micCommands[]` as `System_Microphone`. Miss this → **duplicate-symbol link error + ODR collisions on FeatherS3 only** (XIAO builds green and hides it). This is the classic "board-gated code hides compile breaks" footgun.
- **The G2 stream-enable is a lifecycle, not one line.** `audioCaptureStart(G2)` arms the ring but never sends `AudioCtrCmd{en=1}` → silent dead mic. Need: enable on start, **disable (`en=0`) on stop**, **teardown in `onDisconnect`** (today it does ZERO mic teardown — leaves ring allocated, lease held, `audioReadPcm` returns 0 forever), **re-arm on reconnect**, and an idempotency flag (`gMicStreamOn`).
- **Availability predicate is new + asymmetric.** PDM availability is compile-time (`ENABLE_MICROPHONE_SENSOR`; I2S/PDM is unprobeable, soldered). G2 needs a **new** exported `g2LeftConnected()` = `gL.connected && gL.audioNotifyChar != nullptr`. Do **not** use `isG2Connected()` (OR-of-temples → false-positive when only RIGHT is up, which emits nothing) or `g2BothConnected()`.
- **`audioSetSource` refuses while a capture owner is held** → routing `setmicsource → audioSetSource` becomes a silent no-op mid-run. Must be `stop → audioSetSource → start`.
- **ESP-SR is dormant.** `ENABLE_ESP_SR` is `#define 0` on all boards; the whole `gSrMicSource`/`setmicsource` split-brain compiles out of every shipping binary. A green build proves **nothing** about Requirement 1 — validate with `-DENABLE_ESP_SR=1` on both a SENSOR=1 and SENSOR=0 target.
- **Confirmed correct:** exclusive single-owner concurrency (VU-while-SR reads the owner's *published scalar*, never a 2nd PCM stream); the `HAL_Input` analogy with lease+destructive-pull divergence (not cache).

## Overlooked consumers folded in
- **ESP-NOW `REMOTE_SENSOR_MICROPHONE`** — `CAP_FEATURE_MICROPHONE` is a compile-time bit that won't advertise a runtime-available G2 mic on FeatherS3; remote status payload needs `source`/availability fields.
- **`G2_Page_TestSuite.cpp:2903`** arms the G2 ring directly, bypassing the HAL lease (destructive-drain race).
- **`buildMicReadoutText` (`G2_Glasses.cpp:4204`)** calls PDM-gated `getAudioLevel()` → returns 0 on FeatherS3, the very board where G2 is the only source.

## Corrected design
- **Source model:** one truth = `gAudioSource`, default `AUDIO_SRC_NONE`. `gSrMicSource` deleted; ESP-SR consults `audioGetSource()`/`audioReadPcm()`. `gSettings.micSource` persists a **preference** `{auto,pdm,g2}` (default `auto`), resolved **lazily at every `audioCaptureStart`** against availability — never bound at boot. G2 is LEFT-only (`AUDIO_SRC_G2_LEFT`); RIGHT never a source.
- **Availability:** `audioSourceAvailable(src)` — PDM iff `ENABLE_MICROPHONE_SENSOR`; G2 iff `g2LeftConnected()`. `audioSetSource` rejects unavailable. 0 available → mic UNAVAILABLE. Mid-session disconnect → `onDisconnect` forces `audioCaptureStop` + AFE disarm + file close (non-blocking takes) + lease release. Boot autostart gated on `audioAnySourceAvailable()` with a defer-until-available hook.
- **API surface (HAL_Audio):**
  - `enum AudioSource { AUDIO_SRC_NONE=0, AUDIO_SRC_LOCAL_PDM=1, AUDIO_SRC_G2_LEFT=2 };`
  - `bool audioSourceAvailable(AudioSource); bool audioAnySourceAvailable(); size_t audioListAvailableSources(AudioSource*, size_t);`
  - `bool audioSetSource(AudioSource);` (rejects unavailable + rejects while owner held)
  - `audioCaptureStart(owner, rate)` — resolves NONE→only-available; G2 branch does `g2MicStreamEnable(true)+g2MicSetAfeFeedActive(true)`; fails if none available.
  - `audioCaptureStop(owner)` — G2 branch does `g2MicStreamEnable(false)+g2MicSetAfeFeedActive(false)`.
  - `int audioReadPcm(...)` unchanged (forks per-source).
  - New in G2_Glasses: `bool g2LeftConnected();` (+ G2-off stub) and `bool g2MicStreamEnable(bool)` (single-arm `sendEnvelope(gL,...)`, idempotent, propagates failure).
- **Recording:** `System_Microphone`'s `audioReadPcm` path is the ONE canonical recorder, ungated to `ENABLE_MICROPHONE`. WAV stamps the active source's format (G2 → force 16k/16/mono, ignore `micSampleRate`/`micBitDepth`). `g2micrec` kept as explicit diagnostic; `g2micwav` deprecated once canonical path records G2 everywhere; **`cmd_g2mic` retired** (stale "not wired" text + RIGHT spray).
- **UI:** `Source` selector from `audioListAvailableSources()` (single G2 entry = "G2 (left)"); MIC row gated on `audioAnySourceAvailable()` at runtime; labels/format switch PDM/G2 by active source; status JSON (+ ESP-NOW remote payload) gains `"source"`.
- **Settings:** register `micSource` as a real `SettingEntry`; also fix pre-existing false-`(saved)` for `microphoneSampleRate/Gain/BitDepth`.

## Implementation sequence (order matters)
1. `System_BuildConfig.h`: define `ENABLE_MICROPHONE` (correct formula).
2. **Widen HAL_Audio gate FIRST** (outer → `ENABLE_MICROPHONE`, inner PDM → `ENABLE_MICROPHONE_SENSOR`); add availability API + NONE default + capture-state mutex. *(Prerequisite for every unified caller — else undefined symbols on FeatherS3.)*
3. `System_SensorStubs.{h,cpp}` lockstep flip to `#if !ENABLE_MICROPHONE`.
4. `G2_Glasses`: `g2LeftConnected()`, `g2MicStreamEnable()`, onDisconnect teardown, reconnect re-arm; wire enable/disable into HAL start/stop; retire `cmd_g2mic`.
5. `System_Microphone`: re-gate; source-aware WAV/settings/status; `micConnected := active-source-available && capturing`.
6. UI + registry + remote: `G2_Page_Sensors` selector, `FeatureRegistry` `isAvailable`, ESP-NOW payload, OLED/Web/`System_Utils`/`System_I2C` re-gate.
7. `System_RamFlush` + `HardwareOne` autostart re-gate + typo fixes + defer-until-available hook.
8. ESP-SR unification (dormant; validate with `-DENABLE_ESP_SR=1`).

## Verification gates
- **Build BOTH `ARDUINO_UM_FEATHERS3_DEV` (PDM off) and XIAO ESP32S3 Sense (PDM on)** for every change — XIAO-green proves nothing for FeatherS3.
- Additionally build `-DENABLE_ESP_SR=1` on a SENSOR=1 and a SENSOR=0 target for the ESP-SR delta.
- HW: G2-only FeatherS3 — connect glasses → MIC row appears → select G2 (left) → level + record work → disconnect mid-record → clean stop (no infinite read-zero) → reconnect → re-arm. Watch `g2MicAfeOverrunCount` under BLE load (wake-word starvation is HW-measured, not code-reviewable).

## Deferred
- "Hey Even" wakeword-gated mic toggle. The old `sid=0x0D src=7/code=224`
  interpretation is retired: sid `0x0D` is SyncInfo app lifecycle and does not
  prove a wakeword gesture. This remains deferred until a typed sid `0xE0` or
  other wire event is captured and correlated. Payoff: capture speech + answer
  via our LLM.
