# Shared transcript qualification — 2026-09-28

The shared transcript feature is implemented for local P4 STT and Pi-backed
Dictation. The P4 firmware has been built, flashed app-only, verified and tested
with its onboard microphone. The S3/Pi provider has host-test coverage; a Pi was
not available for physical qualification. Usage is in [TRANSCRIPTS.md](TRANSCRIPTS.md).

## Installed image

- Profile: `transcripts-app-v1`; ESP-IDF 5.5.5, same pinned model and dependencies.
- SHA-256: `7c8e2a54a75ad0daf80d2169ad99c0a7a0d3c40e569040241b667acc48debc35`.
- Application: 6,375,872 bytes; unchanged partition: 6,377,472 bytes; **1,600 bytes free**.
- All 7,959 source-file checks passed. The saved image includes its source
  manifest, ELF, configuration and partition table in the ignored private area.
- The first complete feature image exceeded the app partition by 1,104 bytes.
  Retiring the inherited experiment-only `camerajpegprobe` command recovered
  2,704 bytes. Production camera/JPEG support remains enabled. The previous
  optional R1 health web-panel exclusion remains; no additional standard
  HardwareOne feature or compiler optimization was changed.
- Existing models, filesystem and partition layout were preserved. Neither the
  S3 nor C6 firmware nor the primary checkout was changed.

## Physical checks

| Check | Result |
| --- | --- |
| Saving on; toggled off after 15 s | 71.2 s capture, 7 accepted/saved chunks, one finalized 595-byte file. |
| Saving off; toggled on after 15 s | 46.3 s capture, 5 accepted chunks, no transcript file. |
| Saving on; silence | 8.4 s observation, no chunks and no empty file. |
| Bounded recording | 8 s input, one saved result, finalized 183-byte file. |
| Retry/read/acknowledgement | Repeated reads and acknowledgements did not duplicate saved lines. |
| Camera concurrency | Three successful frame captures during the enabled continuous session. |
| Reboot | Both files retained their SHA-256 hashes; setting remained enabled. |
| Existing configuration | All 20 saved-setting and peripheral postchecks passed. |

Both continuous runs reported zero audio overruns and overlapped capture with
inference. The saving run retained at least 6,312 bytes of worker stack and
2,924 bytes of capture-task stack; bounded mode reported 7,816 worker-stack
bytes after transcript finalization. These measurements qualify this workload,
not every future microphone, storage or concurrency combination.

Downloaded file bodies matched accepted result lines and the final end marker.
The test applies existing credential redaction to both comparison inputs; raw
file hashes independently verify unchanged bytes across reboot. The protected
`fileview` broadcast path refused the file and `fileread` retrieved it through
the authenticated response. No recognized words or credentials are included in
this report. Machine-readable checks are in
[transcript-validation.json](transcript-validation.json).

One preliminary optional-camera run attempted capture before opening the camera;
the helper safely cancelled that session. The complete run above opened the
camera first. This was a test setup correction, with no firmware change.

## Host coverage and limits

Actual production-source tests passed for the shared writer with local-only,
Pi-only and combined flags, one-shot and continuous STT, and both Dictation
providers. ASan/UBSan checks cover identity/epoch capture, setting latching,
accepted-result saving independent of UI consumption, replay, cancellation,
queue ownership, account recreation, capacity and I/O failure behavior.
The concurrent continuous broker also passed ThreadSanitizer.

Actual filesystem policy/listing tests cover owner restrictions, ordinary-file
compatibility, scopes and FAT aliases. Accountless ESP-NOW LIST/STAT/GET rejects
private transcript paths. Private fileview refusal and fileread envelope bounds
passed the BLE on/off and PSRAM/fallback matrix. Serial, shared-output, BLE
trace/history and direct-web response privacy tests passed. Settings persistence
contracts passed; the registry checker adds no new diagnostics beyond four
pre-existing HT16K33 command-key diagnostics.

Physical Pi, SD, OLED and G2 qualification remains outstanding. The current P4
image uses internal storage. Saving is bounded by free storage; full media or an
I/O error stops saving visibly without stopping recognition. Existing saved text
survives cancellation. Files are plain text, and flush/write-size checks do not
guarantee the latest bytes survive sudden power loss. Model accuracy and
segmentation are unchanged; earlier recognition limits still apply.

The tested P4 is left with transcript saving **enabled**, microphone/camera/SR
stopped, voice commands disarmed, and all USB test coordinators closed. New
installations default to saving off. Work remains local to the investigation
branch; no push or pull request was made.
