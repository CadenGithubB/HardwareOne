# Changelog

All notable changes to this project are documented here. The format follows
Keep a Changelog (https://keepachangelog.com) and this project uses
Semantic Versioning (https://semver.org).

Entries for 0.96.1 and earlier were backfilled from git history (this repo had
no tags or releases before 0.96.2); they are terse, commit-grounded summaries,
dated from each version's commit. Dates are YYYY-MM-DD.

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
