# Changelog

All notable changes to this project are documented here. The format follows
Keep a Changelog (https://keepachangelog.com) and this project uses
Semantic Versioning (https://semver.org).

Entries for 0.96.1 and earlier were backfilled from git history (this repo had
no tags or releases before 0.96.2); they are terse, commit-grounded summaries,
dated from each version's commit. Dates are YYYY-MM-DD.

## [0.98.2] - 2026-07-12
Automations can now check far more of the device's own state, the trigger form is simpler, and a multi-trigger scheduling bug is fixed.
### Added
- Automation conditions can check about 30 new things, usable in both the "fire when" condition and IF/THEN command logic: battery percent, free memory (heap, PSRAM, storage), uptime, chip temperature, hour of day, day of week, and whether the clock is synced; WiFi state and signal strength, Bluetooth state, and GPS fix/speed/satellites; the on-device model's state; and a range of ESP-NOW/bond signals - bond online/synced/paired/role, bond link signal strength, the bonded peer's free memory and uptime, whether pairing mode is open, how many mesh peers are known, and the oldest peer's heartbeat age.
### Changed
- The automation form now shows one unified "Triggers" list - the main trigger plus a quiet "+ Add trigger" button for extra ones (up to 4, auto-numbered) - replacing the separate always-visible "Additional Triggers" panel.
### Fixed
- Automations with more than one trigger now fire on each trigger's own schedule. Previously only the first trigger's schedule was honored, so an added trigger set to fire sooner than the main one never ran on its own cadence, and the scheduler repeatedly re-read the automations file until the main trigger came due.

## [0.98.1] - 2026-07-12
Fixes to the on-device LLM's domain refusal gate: it stops refusing basic help questions, and the web chat now shows the refusal message for off-topic prompts instead of going blank.
### Fixed
- The domain refusal gate no longer refuses universal help and identity questions like "what are you", "what do you do", or "help" - any model can answer those even though they contain no topic word. Works on the web, the phone app, and the OLED.
- Web chat now shows the model's refusal message for an off-topic prompt instead of a blank reply. The phone app and OLED already showed it.

## [0.98.0] - 2026-07-11
The on-device LLM comes online with guardrails that keep it on-topic and controls to steer its answers, plus a round of auth-store hardening and crash fixes.
### Added
- On-device LLM is now built into the firmware: load a tiny model from LittleFS or SD and chat with it from the web, OLED, CLI, or over BLE.
- Domain refusal gate: a model can carry a list of its own topic words and now declines a prompt that mentions none of them (with a custom refusal line) instead of inventing an answer.
- Answer controls you can tune per model or per message - a confidence gate that prefixes "I'm not sure, but" when the model is unsure, an n-gram blocker that breaks phrase-repeat loops, and a content boost that keeps replies on topic; the phone app can override sampling per message.
- Model info card: a loaded model can carry an icon and a short description, shown on the OLED ready screen.
- New `bootcount` command reports boot count, crash count, and last reset reason.
- Files can transfer to the phone as raw binary over the secure BLE channel instead of base64 (about a third smaller), speeding up offline map downloads.
### Changed
- Web chat and CLI now fall back to your saved LLM settings for any field left blank, instead of forcing fixed defaults.
- Casually typed names (like "bulbasaur") are matched to the model's trained form, so casing no longer hurts answers.
- KV cache defaults to FP16, giving about twice the context in the same memory.
- Re-saving a known Wi-Fi network with a blank password now keeps the stored password instead of clearing it.
### Fixed
- GPS could corrupt the I2C bus when it collided with other sensors; reads are now serialized through the shared bus lock and served from a cached snapshot, and a southern/western-hemisphere sign error in the cached position is fixed.
- Sensor tasks low on stack could hard-fault; they now shut down cleanly instead.
- Large file reads no longer truncate the reply during map downloads.
- The offline map tile cache is now shared safely between the OLED and the glasses.
### Security
- Auth-database hardening against secret loss: the boot counter moved to its own NVS partition so it no longer rewrites the whole user database every boot (a power cut in that window could wipe all logins). Every user-database write is now atomic. Device-key handling self-heals across key derivations, validates decrypted padding strictly, and refuses to overwrite a still-recoverable secret with an empty value after a failed load.
### Docs
- New specs and plans: per-message LLM generation overrides (app integration), an exact-answer retrieval-hybrid plan, and an LLM settings/control-surface audit; updated the BLE secure-channel framing doc for the new binary frame.

## [0.97.5] - 2026-07-07
A much clearer GPS-track experience on the web: readable start/end/direction and stitched multi-log day-tracks.
### Added
- GPS Tracks (web): a loaded track now shows a green START circle and a red END square, each labelled, plus direction-of-travel arrows along the path - so where you began, where you finished, and which way you went are obvious at a glance.
- GPS track stitching: pick several logs in the order you want and merge them into one continuous day-track (`gpstrackmerge` CLI, or the Stitch panel on the web Maps page) - for an outing a power interruption split across several files.
### Changed
- GPS tracks can hold up to 10000 points (was 500); the buffer lives in PSRAM (~117 KB) - about 14 hours of continuous logging at the default 5-second interval.
### Fixed
- Web Maps: the GPS Tracks Load and Stitch buttons (and camera video delete) now quote the file path, so names that need quoting work instead of failing with "cannot load file".
- Stitching or saving a track now writes under the captures folder with the right permissions (was failing with "cannot create output").
- Loading a track over the web now runs with the caller's identity, fixing a "file not found" when the file was really there.
- The track reader now skips a GPS line that has no fix even when other sensors are co-logged on the same line, so a stray comma can't inject a bogus point.

## [0.97.4] - 2026-07-07
Clearer, more legible offline maps on the glasses and OLED, and no more missing chunks.
### Added
- Map feature differentiation on the glasses and OLED: roads render with a thick-to-thin weight hierarchy (highway / major / minor) and each class has a distinct line style, so the map is far easier to read. The G2 lens additionally uses 16-level green shading and surfaces water bodies and coastlines (drawn cleanly, without tracing the tile grid).
### Changed
- G2 glasses maps render at the lens's native 288x144 in 16-level grayscale, instead of a 128x64 1-bit page upscaled 2.25x - crisp uniform lines and feature classes that stand out. The OLED stays 128x64 but now shares the road weight hierarchy and line styles (it can't show brightness at 1-bit, but thickness and pattern read fine).
- Larger default map tile cache: 1.25 MB, up from 1 MB (PSRAM).
### Fixed
- Chunks of the map no longer go missing. Tiles larger than the tile cache's biggest size class were silently dropped every frame, so dense and coastal areas showed rectangular holes; the cache now always sizes its top tier to the largest tile in the map (the generator caps tiles at 20 KB).
### Docs
- Updated the G2 map multi-shade rendering plan for the shipped native-resolution renderer and the OLED / water-coast follow-ups.

## [0.97.3] - 2026-07-06
Maps on the G2 glasses, WPS-style ESP-NOW pairing, and a batch of OLED, web, and build fixes.
### Added
- G2 glasses: a "Maps" page under a new "Apps" hijack submenu renders the offline map to the lens as a list+image compound (Zoom In/Out, Reset View, Recenter); the `g2map` CLI opens it too. GPS and RTC re-enabled.
- ESP-NOW WPS-style pairing mode: open a timed window (`espnowpairmode`, or the OLED ESP-NOW > Pairing screen) on two same-mesh devices and they broadcast a discovery beacon and auto secure-pair. Admin-only; refuses without a mesh passphrase.
- OLED CLI Input page shows an inline OK/FAILED result screen after each command.
- OLED Power menu: "Restart Device" moved to the top menu behind a Yes/No confirmation.
### Changed
- Setup wizard timezone list shows a representative city (e.g. "US Eastern - New York") instead of only an abbreviation.
- Web logging page: the debug-flag checkbox pane is now wired to its select-all/none and flag-collection JS.
### Fixed
- G2 hijack map and camera pages refresh the 60s safety-timeout on every control tap, so active use no longer drops back to the menu mid-interaction.
- OLED Change Password: the on-screen keyboard now draws when a field is selected (was active but never rendered).
- OLED ESP-NOW settings/device-config keyboards exit on a single B press (was leaving a title-less ghost keyboard).
- Build: prefer `-DIDF_TARGET` over a stale sdkconfig so `idf.py set-target` can switch boards without tripping the target-mismatch guard.
### Docs
- Planning docs: sensor reading-envelope / envelope-cleanup / rendering-unification, ESP-NOW pairing-mode brief, a settings.json lifecycle audit, and a G2 map multi-shade rendering plan.

## [0.97.2] - 2026-07-03
Durable system event log: the firmware now keeps a persistent, always-on record of what it does at the system level, so an unexpected event (like an odd startup message) can be read back from the device instead of reconstructed after the fact.
### Added
- logSystemEvent() writes always-on [EVENT][CAT] lines to /system/sys_logs/system-events.log (256 KB capped ring), independent of debug flags and viewable from the web logging page. Early-boot events are held in a small buffer and flushed once the log system is up.
- Lifecycle coverage across boot, filesystem (mount/format, file deletions, orphan cleanup), settings load/save/failures, WiFi, MQTT, HTTP/HTTPS server, ESP-NOW init and mesh/bond peer online-offline, users, I2C buses and sensors, camera, OLED, voice (SR), LLM, automations, and every reboot path - logged in both directions (came up / went down) where it applies.
- Per-boot orientation divider ("Device Powered On | boot #N | reset=...") written to the login, i2c, and error logs so each log file reads correctly on its own.
### Docs
- New docs/AUTH_LOG_FORMAT.md documents the boot divider and time-sync anchor lines.
- Sensor JSON comments aligned with the unified reading envelope (valid/connected/ts fields).

## [0.97.1] - 2026-07-01
Sensors web page: a remote (ESP-NOW peer) sensor now renders as a readable card instead of a raw JSON dump.
### Added
- Shared `hwRenderGenericSensor` labeled-field card for remote sensors (ToF, thermal, GPS, RTC, presence, IMU), replacing the raw `JSON.stringify` fallback.
### Changed
- Remote gamepad / rotary-encoder now reuse the local card renderer (parameterized `hwRenderAnoState` + a shared `hwBuildAnoInner`); removed the duplicate `hwRenderRemoteInput`.

## [0.97.0] - 2026-07-01
Unified sensor reading format: every sensor now shares one envelope shape, and the bodies were trimmed to just the measurement.
### Added
- Shared reading envelope (`valid`, `connected`, `ts`, `age`) at the head of every sensor's data, via `sensorEnvelopeBegin()`.
- Thermal min/avg/max summary reading (`thermalread json`), also embedded in `sensors json` like the other sensors.
- FM radio readings now carry a real `ts`/`age` (added a `lastUpdate` to its cache).
### Changed
- Sensor bodies trimmed to measurement-only: dropped bookkeeping (`seq`, `total_objects`) and device-state already in the discovery layer (`enabled`, IMU init flags, APDS mode flags).
- ToF emits only detected objects (was 4 fixed slots padded with nulls); dropped the redundant `distance_cm`.
- Not-ready / error readings now report `valid:false` instead of an `{"error":...}` shape; renamed `val` -> `valid` and `timestamp`/`ageMs` -> `ts`/`age` where they differed.
### Fixed
- ToF JSON builder could underflow its remaining-length; APDS read its cache without holding the mutex.
- Disabled the thermal ESP-NOW broadcast: the 768-pixel frame never fit the 200-byte packet limit, so it was built and silently dropped every second.

## [0.96.3] - 2026-07-01
Small CLI fixes: imagesend needs an explicit path, and espnowbroadcast flags no-peers as an error.
### Changed
- `imagesend <device> "<path>"` now requires an explicit path (removed the implicit "send the most recent image").
### Fixed
- `espnowbroadcast` with no paired devices now returns an `Error:` message instead of a plain string.

## [0.96.2] - 2026-06-28
CLI self-documentation: the built-in `help` now describes every module and command accurately.
### Added
- Per-module subsystem overviews printed atop `help <module>` (all 43 modules).
### Changed
- ESP-NOW and bond commands now state when they are asynchronous and name where the result lands (`espnowmessages json`, `espnowtoporesults`, `bondshowremotemanifest`, and so on); fire-and-forget sends say "no reply".
- Fixed stale usage strings: `log` (added `autostart`), `sensorlog` (added `interval`/`autostart`), `power` (real per-subcommand syntax).
### Fixed
- Corrected range bounds: `ledBrightness` (0-100), `tzoffsetminutes` (now reaches UTC+13/+14), `oledBootDuration` (500-10000 ms), and board-derived i2c SDA/SCL pin limits (39 on ESP32-classic, 48 on ESP32-S3).

## [0.96.1] - 2026-06-24
G2 on-device image decode and display-mode robustness.
### Added
- G2 glasses: decode and display JPG/BMP files on-device without a camera sensor; app partition grown to fit.
### Fixed
- OLED Bluetooth and Remote-Settings mode files anchored so `--gc-sections` no longer strips them.

## [0.96.0] - 2026-06-23
Guided recovery after a partial restore.
### Added
- Guided post-restore login flow and cross-device WiFi preservation.
### Docs
- Expanded CLI usage and settings documentation.

## [0.95.13] - 2026-06-21
### Changed
- User-sync now authenticates against the receiving device's admin; synced users land as standard role; `createdBy`/`createdAtSource` split.

## [0.95.12] - 2026-06-20
### Added
- ESP-NOW deferred-writer big-file streaming (chunk-87 fix); espnow-core cleanup.

## [0.95.11] - 2026-06-19
### Added
- JSON command surface; unified auth logging.
### Fixed
- espnow/bond audit; bonded-CLI socket fix.

## [0.95.10] - 2026-06-19
### Added
- Peer metadata over BLE/CLI with a shared serializer; chunked CMD results.

## [0.95.9] - 2026-06-18
### Changed
- G2 ESP-NOW messages UI brought in line with the chunked message store.

## [0.95.8] - 2026-06-18
### Fixed
- Reliable device-to-app secure-channel delivery and framing (BLE).

## [0.95.7] - 2026-06-14
### Changed
- JSON schema-version field renamed `v` to `schema`; unified sensor error shape.

## [0.95.6] - 2026-06-14
### Added
- On-device LLM registered as a first-class feature with auto-start; JSON list-command envelope; HW_BOARD board files extended to ESP32 boards.

## [0.95.5] - 2026-06-09
### Changed
- Migrated to ESP-IDF 5.5.1 (from 5.3.1), including the new `i2c_master` driver.

## [0.95] - 2026-06-01
### Added
- Per-transport session idle-logout, plus accumulated fixes.

## [0.94] - 2026-05-25
### Added
- ANO rotary-encoder support; OLED input unification.

## [0.93] - 2026-05-10
### Added
- Per-sensor sub-flag debug system.
### Changed
- Settings JSON restructure.

## [0.92] - 2026-04-24
### Added
- Video recording.
### Changed
- Logging hardening; SD overflow routing.

## [0.91] - 2026-04-03
### Fixed
- NTP timestamp resolution.
### Changed
- Debug-flag cleanup; UI color variables.

## [0.9] - 2026-03-14
### Added
- Version single-sourced from CMakeLists.txt (`PROJECT_VER`) and surfaced at runtime: boot banner, `status` CLI, OLED, settings JSON, `/api/ping`, and backup metadata.
