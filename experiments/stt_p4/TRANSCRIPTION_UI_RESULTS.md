# Transcription interface qualification — 2026-09-28

## Scope

G2 Apps, OLED Apps and the collapsible Microphone section on Web Sensors expose
the existing provider-neutral Dictation and transcript core. Recognition,
segmentation, model weights, microphone HAL and transcript format are unchanged.
The shared additions are an exact application lease, retry-safe last receipt,
owner-scoped status/recovery, and bounded private file commands. Pi supervision
uses its existing worker even when a frontend is closed.

Usage and UI limits are in [TRANSCRIPTION_UI.md](TRANSCRIPTION_UI.md).
Machine-readable evidence is in [transcription-ui-validation.json](transcription-ui-validation.json).

## Host qualification

Actual production-source tests passed with AddressSanitizer and
UndefinedBehaviorSanitizer:

- `test_dictation_local.py`: local and Pi provider branches, feature-off stubs,
  keyboard/App separation, exact owner/epoch/exchange checks, last-ACK retries,
  terminal saving state, logout and Pi deadlines without frontend polling.
- `test_dictation_ui.py`, `test_stt_g2_status.py`: existing keyboard/status paths.
- `test_transcription_ui_adapter.py`: actual parser, path normalizer and JSON
  library; account-only internal/SD files, traversal rejection, eight-file
  listing pages, 512-byte UTF-8 windows, errors and revocation.
- `test_g2_transcription.py`, `test_g2_hijack_epoch.py`: actual page/bridge;
  navigation, ownership, stale callbacks, ambiguous-start recovery, exact
  receipts, file paging, setting failures and asynchronous admission.
- `test_oled_transcription.py`: actual mode and command bridge; callback mailbox,
  late-start cleanup, stale view results, piece joining, ACK retry, file windows,
  bounded history, session wiping and display-width clipping.
- `test_transcription_web.py`: actual HTTP handlers and real ArduinoJson;
  cookie authority, revocation, bounded forms, owner recovery and busy status.
  Actual embedded browser JavaScript also passes behavior tests for controls,
  lost replies, collapsed-panel draining, logout, safe text rendering, bounded
  previews/files and saving policy.
- `test_transcription_activity.py`: exact passive poll/ACK classification;
  automatic traffic neither wakes the device nor fills command audit logs.
- `test_transcription_ui_policy.cpp`: strict ID/offset parsing, UTF-8 bounds,
  recent-tail eviction, filename construction and buffer wiping.

The OLED registry remains within its existing capacity (57/64 entries at the
static all-feature upper bound). G2 Apps/page capacities include the new entry.

## Firmware and hardware

- Installed P4 image: `transcription-ui-v1`, SHA-256
  `20a22d79b0ed9656aefa13ca6ff9108739a2db7e3ab2053adc38108910090f4d`.
- Application: **6,362,416 bytes**, with **15,056 bytes free** in the unchanged
  6,377,472-byte partition. All **7,972 source checks** passed. App-only write and
  readback verification passed; boot showed no panic.
- All **20 existing P4 configuration/peripheral postchecks** passed without an
  additional reboot. Models, saved settings, saved files, partition layout and
  C6 firmware were preserved. Microphone/camera/SR remain stopped; saving remains
  enabled, matching the earlier qualification.
- Physical USB adapter checks passed: local provider becomes available after its
  lazy model check, two existing transcripts list correctly, a saved 183-byte
  transcript reads with its prior SHA-256, other-account paths are rejected,
  and missing SD is reported without inventing an empty mounted card.
- A preliminary USB check expected model availability immediately; it was
  correctly `checking local model`. The completed check waits for the existing
  asynchronous probe, as the polling frontends already do.

The first linked UI image was 40,128 bytes over the partition even with the
standalone BLE diagnostic temporarily excluded. The final image retains that
diagnostic and every existing feature. One existing ESP-NOW script is served as
an exact gzip asset: 82,734 source bytes become 17,787 compressed bytes. This is
chip-independent and uses browser decompression, with no new runtime dependency.
The source/header consistency gate rejects drift; all 45 web-UI tests passed,
including the actual asset handler under ASan/UBSan. The script bytes and normal
synchronous execution order are unchanged.

The original P4 image and new image/ELF/manifests are preserved in ignored private
storage. The S3 was not reflashed or moved onto a P4 test network. At the user's
request its existing Wi-Fi/web service is left available for concurrent OpenClaw
work; see STATUS.md for current state and serial access guidance. P4 end-to-end
web-network testing through the S3 was deferred to preserve that connection.

The connected P4 has no display, and physical G2, SD and Pi accessories are not
available for qualification. Those interfaces/providers have host coverage;
this report does not claim physical gesture, panel, SD or Pi testing.
