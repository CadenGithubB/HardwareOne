# Compatibility and upgrades

What a HardwareOne firmware version expects from the things around it, and what an update
keeps. Keep this file in step with CHANGELOG `[Unreleased]`.

## Companion and peer versions the firmware is built against

| Peer | Required / supported | Where it is checked | Notes |
|---|---|---|---|
| ESP32-C6 radio companion (P4X-EYE, Waveshare P4) | ESP-Hosted-MCU slave **2.12.13**, exact, with the HardwareOne ESP-NOW bridge | `c6status` reports `Hosted 2.12.13 (qualified)`; the P4 refuses an unqualified companion for radio work | A stock or Waveshare-factory C6 must be flashed with `c6update` from `tools/p4/companion` before Wi-Fi, Bluetooth or ESP-NOW work. Legacy companion compatibility is limited to that explicit update (0b3d0348). |
| Phone app (Android) | phone link **v2** (`APP_JSON_CONTRACT.md` section 11.9), phone pipe **HW1-PIPE-v1** (`PHONE_PIPE_HANDOFF.md`) | `bleinfo json` / `blestatus json` publish the link version; the pipe handshake carries its info string | Older v1 apps see the console-only path. |
| CM5 / Raspberry Pi AI service (XIAO-S3 + Pi builds) | the UART link protocol in `System_UartLink.cpp`, mirrored byte for byte by `cm5/ai-service link/protocol.py` | frame validation on both ends | Ship both sides from the same commit; there is no negotiation. |
| Even G2 glasses | the Even G2 BLE profile as exercised in the dated hardware records (`docs/G2_*.md`) | discovery and page protocol checks in `G2_Glasses.cpp` | Glasses firmware updates by Even can change pages; the records name the glasses firmware they were taken with. |
| Even R1 ring | advertised name `EVEN R1_XXXXXX` (six hex digits) | `classifyRingName` in `G2_Glasses.cpp`, mirrored in `G2_Ring.cpp` | Any other name is ignored by the ring scan. |
| Arduino core | arduino-esp32 **3.3.5** (`11bc7ac1a`) plus the patches in `docs/arduino-local-patches/` | `tools/vendor_arduino.sh --check`, run by every build script | Not a runtime peer, but the build breaks silently without it (see `docs/arduino-local-patches/README.md`). |
| ESP-IDF | **5.5.5** | `tools/p4/prepare_sdk.sh` (P4) and the build scripts | One IDF for every board. |
| Other HardwareOne devices (ESP-NOW mesh) | **mesh v4 on ESP-NOW v1 frames**: HardwareOne's mesh format, frame version byte 4, within Espressif ESP-NOW v1's 250-byte frames | every received frame's magic and version byte | Mesh devices must run releases with the same format; there is no negotiation. |

`versions` on the device lists these, as this image carries them, in one place: the firmware, the
recovery updater and OTA layout, the ESP-NOW format and stack, the BLE console channel, the phone
link and pipe, the CM5 control protocol, the C6 companion's reported and expected versions, the
local STT model, the ESP-NOW identity and peer file versions, the automations file and backup
bundle formats and the ESP-IDF and Arduino core it was built with (`versions json` for tools;
`BUILD_INFO.md` in a build directory has the build-time half).

## Persisted data and what an upgrade keeps

- `settings.json` carries no format version; it records the `firmwareVersion` that last wrote it.
  Unknown keys are kept; a missing key takes its default.
- `automations.json` carries `"version": 2` (`kAutomationsFileVersion`, `versions` key
  `automations_file`). Every read checks it. A file with no `"version"` or with version 1 loads as
  before: the reader has never treated the versions differently (an entry from the old flat or
  `schedule` layout, without a `triggers` array, still has no trigger and does not fire). A higher
  version is from newer
  firmware: its automations are not loaded or run, and the file is left unchanged (nothing is written
  back from it), so that firmware finds it as it left it. The console says so once. `automation`
  commands, the web Automations page (`GET /api/automations`: `success` false with
  `formatVersion` and `supported`) and its exports (`409`) name the version instead of listing it.
- The web migration backup bundle (`HWBACKUP`) carries `"formatVersion": 1` (`kBackupFormatVersion`,
  `versions` key `backup_format`). Import from Backup refuses a higher or non-whole-number
  `formatVersion` before staging anything (`400`, `"error":"Backup format too new"` with
  `formatVersion` and `supported`). A bundle without the field is format 1.
- **An app update** (OTA or cable app-only write) keeps settings, user accounts, saved Wi-Fi networks
  and automations. A renamed setting or event name gets an alias, or the change is listed under
  "Changed" in CHANGELOG with the migration step.
  - `g2_worn` (event) was renamed `g2_plugin_alive` in 64713c0d; saved automations and notification
    rules that still say `g2_worn` resolve to the new kind (alias in `systemEventKindFromName`).
  - `c6_console` (command) is an alias of `c6console`.
- **A full erase** (`idf.py erase-flash`, then the first-time setup) removes settings, accounts and
  everything else stored on the board. A board coming from a different partition layout needs one
  (see QUICKSTART). On a recovery OTA layout (`HW_OTA_LAYOUT=1`) the erase also removes the recovery
  updater and the recovery credential, so the install after it is steps 3-4 of
  `docs/OTA_DEVICE_SETUP.md`: `migration-flash` (`encrypted-migration-flash` on `feathers3_fe`), then
  `setpin` or `otapin`.
- OTA layout ids (`hw1-f3-ota-v1`, `hw1-f3fe-ota-v1`, …) and the OTA manifest format keep their
  meaning; a new layout gets a new id rather than a changed meaning.

## Account recovery

A lost admin password on a single-account device cannot be recovered in place (`factoryreset`
needs a super-admin login). A second admin can reset it with `userresetpassword`; otherwise erase the
LittleFS region over the cable, which also removes settings and captures. An owner login file that
can no longer be read (`/system/users/user_settings/1.json`) is moved aside to `1.json.corrupt-1`,
and the next boot asks on the console or display for a new owner password; other accounts and
settings stay as they are. First-time setup refuses
reserved owner names (`system`, `AuthBypass`, the bond admin) and passwords shorter than 6
characters, so a fresh device cannot be set up into a lockout.

## Not promised

- Anything labelled experimental in the README matrix (currently the Waveshare P4 row and any OTA
  layout without a dated PASS record).
- Captured logs, transcripts and sensor logs on the SD card are data, not part of the upgrade
  contract; keep your own copies.
