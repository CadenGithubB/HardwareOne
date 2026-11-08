# HardwareOne 1.0 readiness: P2 re-check, G-62..G-89 (2026-11-08)

Tree: `/Volumes/hw1build/road-to-1.0`, branch `claude/road-to-1.0` at `57f8bbb3`. One skeptic re-checked each group against today's tree. G-73 is left out because it is already fixed.

**Coverage gap:** the verdict input stopped partway through G-82 (its bounded fix is cut off). No verdicts arrived for **G-83..G-89**, so this report does not cover them. They need a separate pass.

## 1. Summary table

| ID | Title (short) | Still real? | User impact (short) | Fix size | Recommendation |
|---|---|---|---|---|---|
| G-62 | Stale build artifacts / partition comment | partly | Low: maintainers read wrong factory/LittleFS sizes in CMakeLists.txt | docs-only | fix before 1.0 |
| G-63 | Base XIAO forced to Sense; LLM "S3-only" wording | yes | Base-XIAO owners get a Sense build; docs wrongly say LLM is S3-only | small code | KNOWN_ISSUES |
| G-64 | Updater version fixed at 1.0.0 | yes | Updaters in the field can't be told apart; future min-updater gating impossible | small code | fix before 1.0 |
| G-65 | OTA rough edges (pair-audit deps, erase-flash) | partly | Fresh-build ota0-flash fails; stock erase-flash wipes recovery updater | small code | fix before 1.0 |
| G-66 | No Wi-Fi re-hunt across saved networks | yes | Device stays offline after moving to another saved network until openwifi | docs-only (1.0) | KNOWN_ISSUES |
| G-67 | P4X console-hang "Under investigation" never closed | yes | Release record looks unresolved | docs-only | fix before 1.0 |
| G-68 | Non-atomic / unchecked writes (ESP-NOW identity, certs, restore) | yes | Rare: near-full FS can force a bond-wiping regen | small code | fix before 1.0 |
| G-69 | WAV header only at finalize | partly | Power cut loses the clip (max 60 s) | small code | KNOWN_ISSUES |
| G-70 | Interval automations rewrite file every fire | yes | Sub-minute intervals wear flash over months | small code | KNOWN_ISSUES |
| G-71 | Boot-automation leak, no heap_low event, aborting `new` | yes | Small per-boot heap leak; OOM reboots instead of refusing | small code | fix before 1.0 |
| G-72 | Single httpd worker; SSE holds 600 ms | yes | Web UI lag; uploads/slow clients block for 20/30 s | small code | KNOWN_ISSUES |
| G-74 | FeatherS3 idle downclock not hardware-retested | yes | Low: non-default modes + GPS run an unverified S3 switch path | needs hardware | KNOWN_ISSUES |
| G-75 | Dead `powerdim` / displayDimLevel setting | fixed | None (optional CHANGELOG line) | docs-only | close |
| G-76 | G2 case lever deferral | fixed | None; deferral recorded | docs-only | close |
| G-77 | MQTT payload logs; ESP-NOW keys in PSRAM | partly | MQTT fixed; PSRAM key residue undocumented | docs-only | KNOWN_ISSUES |
| G-78 | Restore confirm prompt shows no source details | yes | LAN attacker's forged bundle looks the same as the operator's | small code | fix before 1.0 |
| G-79 | SNTP trusted as-is; R1 retention follows clock | yes | Forged NTP reply wipes all R1 ring history | small code | fix before 1.0 (retention); KNOWN_ISSUES (SNTP) |
| G-80 | `factoryreset` keeps credentials | yes | Device handed over still holds Wi-Fi/MQTT/LLM/BLE/ESP-NOW secrets | docs-only (1.0) | KNOWN_ISSUES |
| G-81 | Stale maintainer docs | partly | Maintainers; 1.0 bump would leave "0.99.99" in configurations/README | docs-only | fix before 1.0 |
| G-82 | Tree weight, unused libs, RE tooling | yes | ~184 MB clone/archive, unused GPL libs; no build impact | medium | defer |
| G-83..G-89 | not received | n/a | n/a | n/a | needs re-check |

## 2. Fix before 1.0

These are real, user-visible and small or docs-only.

- **G-64 Updater version** (small code). In `updater/CMakeLists.txt` set `HW1_UPDATER_SEMVER "1.1.0"` and build `_project_version` as `${HW1_UPDATER_SEMVER}+${_version_suffix}`. Add `factoryUpdaterVersion` to `otastatus json` and the text output (System_OTA.cpp ~:1415, use "unavailable" if the read fails). In RELEASING.md, add a bump rule and pass `--min-updater` through an env var in `build_deployment.sh:180`. Reword CHANGELOG.md:735-737 to name the version otastatus shows. This only helps if it ships before 1.0 updaters reach the field.
- **G-65 OTA rough edges** (small code). Add `partition_table_bin` and `blank_ota_data` to the `hw1-ota-pair-audit` DEPENDS (CMakeLists.txt:1089, inside `if(TARGET)`), then drop or re-date the first paragraph of OTA_DEVICE_SETUP.md:294-298. Add a project-root `idf_ext.py` that refuses `erase-flash`/`erase_flash` on the recovery-OTA layout. If that does not make 1.0, copy the warning into KNOWN_ISSUES. Also note that the Android app must use `slotSize` from `otastatus json`.
- **G-67 Close the P4X console-hang note** (docs-only). Add a line under [Unreleased]: the 0.99.98.4 known issue was closed when phone-client mode was removed in 0.99.98.5, the 2026-10-21/24 four-link benches kept the console working, and the cause was never found. Optionally add "(closed in 0.99.98.5)" at :536. If nobody will claim closure without a retest, run one 15-minute `serial_capture.py` session (blemode client + HTTP + phone pipe). If it reproduces, add a KNOWN_ISSUES row.
- **G-68 Atomic writes** (small code). Check `written != measureJson(doc)` in both identity writers (System_ESPNow_Identity.cpp:110-118, :403-409). On a mismatch, remove the tmp and skip the rename. Write the HTTPS cert and key with `writeTextAtomic` and check the result. Add `/system/espnow` and `/system/certs` to the boot tmp sweep (System_Filesystem.cpp:81). Add a KNOWN_ISSUES line saying restore promotion is per-file.
- **G-71 Leak / OOM / heap event** (small code). Free `cmdsList` before the `continue` in the condition-false branch (System_Automation.cpp ~1186). Change the 11 G2_Glasses.cpp arg-struct and session-job `new` calls to `new (std::nothrow)`; null checks are already there. Add a `heap_low` event row, posted from the Tier 1 memory sample with a 60 s cooldown. If that is too much for 1.0, do the first two now and list the missing event in KNOWN_ISSUES.
- **G-78 Restore prompt details** (small code). When a bundle is staged, save its hostname/board, a short fingerprint, its size and same/other device into a static summary. Add `migrationStagedSummary()` and show it in the serial and OLED prompts (System_FirstTimeSetup.cpp:799-805), still defaulting to No. In `discardStagedMigrationRestore`, clear the flag after the free.
- **G-79 R1 retention** (small code, ~15 lines). In `enforceRetention`, compute the age cutoff from `min(now, newestDay + 1 day)` so a forward clock jump can't delete the newest 30 days. Add a host test with the clock set 10 years ahead. SNTP itself goes to KNOWN_ISSUES (below).

Also marked "fix before 1.0", but only maintainers see these (docs-only, cheap):

- **G-62** Rewrite the CMakeLists.txt:247-251 partition comment from `partitions/*.csv`, or just point at the CSVs. Add a release-checklist step to regenerate BUILD_INFO from the tag commit. Optionally reconcile configurations/README.md:40-41 with the CHANGELOG entry for the p4-glasses-test build.
- **G-81** One commit of about 20 lines:
  - arduino-local-patches/README.md:32 and :74-79
  - APP_JSON_CONTRACT.md:3-6: say which firmware version it covers
  - RELEASING.md:78-79: drop the tag counts
  - configurations/README.md:28: remove "0.99.99" or add the file to the version-site list
  - configurations/README.md:212: list all preset keys or point at the file
  - configurations/README.md:5-18: add the 7 missing profiles
  - CONTRIBUTING.md:40: retitle the section as history

## 3. Add to KNOWN_ISSUES

- **G-63 Board:** a base XIAO ESP32S3 (no Sense) can't be selected; S3 builds assume Sense. Mark BOARD_SWITCHING.md:126 unsupported and drop :309. Fix the LLM wording to "needs PSRAM" (USERGUIDE.md:59, README.md:113, System_BuildConfig.h:1027).
- **G-66 Wi-Fi:** auto-reconnect retries only the same network. The best-saved-network hunt runs only at boot, on `openwifi` and at radio-on. Workaround: an interval automation `IF WIFI=NONE THEN openwifi`. Also add this to USERGUIDE.
- **G-69 Recording:** a recording cut by power loss is lost (at most 60 s).
- **G-70 Automations:** every interval fire rewrites automations.json, so intervals under about 60 s wear flash. Use 60 s or longer.
- **G-72 Web:** there is one httpd worker. Uploads and slow clients can block the web UI for up to 20 s (HTTPS) or 30 s (HTTP), and SSE polls can add about 0.6 s of lag.
- **G-74 Power:** FeatherS3 idle downclock in non-Adaptive modes with GPS is verified on the host only, pending a 30-minute bench soak.
- **G-77 ESP-NOW security:** session keys, the rekey private key, the bond token and the decrypt buffer live in PSRAM, so they can be read with bench access if flash encryption is off. Workaround: enable flash encryption, and re-bond after a device is lost.
- **G-79 Time:** plain SNTP replies are trusted as-is, so a same-LAN attacker can set the clock.
- **G-80 Device handover:** `factoryreset` removes accounts only; Wi-Fi, MQTT, LLM, BLE and ESP-NOW credentials stay. Run `idf.py erase-flash` before giving the device away. Also add this to USERGUIDE ~:433.
- Fallbacks if the code fix slips: G-65 (stock erase-flash wipes the recovery updater; first ota0-flash in a fresh build dir fails), G-68 (restore is not transactional), G-71 (no heap_low event).

## 4. Close

- **G-75 (fixed):** commit `64713c0d` removed `powerdim` and `displayDimLevel` from firmware, web, tests and the command reference. `git grep` finds only readiness reports and experiment captures. Optional: add a CHANGELOG "Removed" line.
- **G-76 (fixed):** the deferral is recorded at RELEASE_READINESS_1_0.md:301/318, and no 1.0 doc promises the feature. The host tools are now tracked (`46f27f19`). Optional: fix the stale "field 13 is skipped" at G2_CASE_PRESENCE_POWER_PLAN.md:47 (f13 has been parsed since `79ceb610` but nothing uses it).
- These parts of open groups are already closed:
  - **G-62:** the stale build dirs are gone. Today's build dirs are clean and gitignored, and Appendix A:156 already discounts untracked BUILD_INFO.
  - **G-65:** the Android `0x5A0000` literal is gone; `otastatus json` now publishes `slotSize`.
  - **G-69:** the premise is corrected. Recordings are capped at 60 s, not 10 min, and the PCM is unlikely to survive anyway because nothing is flushed.
  - **G-72:** usedBytes now comes from the cached VFS snapshot.
  - **G-77:** the MQTT payload logging was fixed in `660c965b`, and `test_mqtt_log_privacy.py` passes.
  - **G-81:** the patches-README re-apply hint and the "untracked" wording are fixed.

## 5. Defer

- **G-82 (medium):** the 184 MB tree, about 52 unused library folders (some GPL), the enclosure STLs and review zips, and the G2/R1 RE tooling. The owner decides what to publish. Removing files shrinks the archive and checkout but not the full clone without a history rewrite. The bounded-fix text was cut off in the input.
