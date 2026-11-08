# Compatibility and upgrade promise

What a HardwareOne firmware version expects from the things around it, and what a 1.x upgrade
keeps. Readiness item P0-7; keep this file in step with CHANGELOG `[Unreleased]`.

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

## Persisted data and what an upgrade keeps

- `settings.json` carries `schema: 1`. Unknown keys are kept; a missing key takes its default.
- **Within 1.x**: settings, user accounts, saved Wi-Fi networks and automations are kept across an
  app update (OTA or cable app-only write). A renamed setting or event name gets an alias, or the
  change is listed under "Changed" in CHANGELOG with the migration step.
  - `g2_worn` (event) was renamed `g2_plugin_alive` in 64713c0d; saved automations and notification
    rules that still say `g2_worn` resolve to the new kind (alias in `systemEventKindFromName`).
  - `c6_console` (command) is an alias of `c6console`.
- **1.0.0 is a full-erase install** for boards coming from 0.99.x: `idf.py erase-flash` first, then
  the first-time setup. Settings from 0.99.x are not promised to carry over.
- OTA layout ids (`hw1-f3-ota-v1`, `hw1-f3fe-ota-v1`, …) and the OTA manifest format are frozen
  within 1.x; a new layout gets a new id rather than a changed meaning.

## Account recovery

A lost admin password on a single-account device cannot be recovered in place (`factoryreset`
needs a super-admin login). A second admin can reset it with `userresetpassword`; otherwise erase the
LittleFS region over the cable, which also removes settings and captures. First-time setup refuses
reserved owner names (`system`, `AuthBypass`, the bond admin) and passwords shorter than 6
characters, so a fresh device cannot be set up into a lockout.

## Not promised

- Anything labelled experimental in the README matrix (currently the Waveshare P4 row and any OTA
  layout without a dated PASS record).
- Captured logs, transcripts and sensor logs on the SD card are data, not part of the upgrade
  contract; keep your own copies.
