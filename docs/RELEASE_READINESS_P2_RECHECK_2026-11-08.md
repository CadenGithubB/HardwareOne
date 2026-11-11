# HardwareOne 1.0 readiness: P2 re-check, G-62..G-89 (2026-11-08)

Tree: `/Volumes/hw1build/road-to-1.0`, branch `claude/road-to-1.0` at `57f8bbb3`. One skeptic re-checked each group against today's tree. G-73 is left out because it is already fixed.

**Coverage:** G-62..G-82 below; G-83..G-89 were re-checked separately (addendum at the end).

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

## Addendum: G-83..G-89 (2026-11-08)

All seven findings were re-checked at 966ee3f1 (`claude/road-to-1.0`), one skeptic per group. Each one is still real. Two need fixing before 1.0, one needs a KNOWN_ISSUES line, and four can be closed for 1.0 and moved to the post-1.0 backlog.

| ID | Title | Still real? | User impact | Fix size | Recommendation |
|---|---|---|---|---|---|
| G-83 | Two hand-synchronised known-task tables already disagree by one row | Yes. It is now committed, so the "uncommitted" tag is out of date. | Low, diagnostics only. `[MEMSAMPLE]` never shows `espnow_tx`. MemoryMonitor reads high-water marks live through captured handles, which TaskUtils deliberately avoids. | Small (about 60-100 lines in 2-3 files plus a host check) | Optional cleanup for 0.99.99.x or after 1.0. Do not hold the release. |
| G-84 | Command handlers registered under two names (11 pairs, now 12) | Yes, with `c6console`/`c6_console` added. The "no-alias policy" framing is overstated, and the proposed direction is backwards. | Small and cosmetic. About 12 extra help and reference rows. Nothing behaves wrongly. | Small for labels only. Medium for real removal. | Downgrade to P3 or defer. Keep the `open<x>`/`close<x>`/`<x>read` convention and label the twins as aliases. Decide removal after 1.0 together with the app clients. |
| G-85 | System_Utils catch-all header pulls WebServer_Server.h into about half the sources | Yes, slightly larger: 105 of 226 `.cpp` files, 5,290 lines. | Developers only. Long rebuilds, hidden dependencies, and the executor and auth gate are buried in a 5.3k-line utility file. | Medium for the full slice. Small for the include removal alone. | Defer past 1.0 (P2 optional). Remove the include first, then move the executor in a separate commit. |
| G-86 | Compatibility shims: dead aliases and live APIs mislabelled legacy | Yes. The live APIs have gained callers. `getClientIp(String&)` has about 9 production callers, not 5. | None for end users. Source hygiene only. | Small (about 6 files, roughly 15 lines, mostly comments) | Optional hygiene: one commit if there is a spare slot, otherwise after 1.0. Do not migrate the `getClientIp(String&)` callers. |
| G-87 | openwifi blocks the shared command executor | Yes, and worse than stated. Up to about 150 s for plain `openwifi` (the best-network hunt), 50 s for `--index` and `--saved-id`. | Moderate when WiFi is unreachable. All transports queue behind `cmd_exec`. The web caller ties up httpd and times out after 60 s while the hunt keeps running. | Medium | Defer the code fix past 1.0. Document the limitation for 1.0 and correct "~50 s" in the group text. |
| G-88 | Command-registry drift gate unusable: audit always exits 1, no reference `--check`, and the reference has now drifted | Yes. The drift has now happened: `docs/COMMAND_REFERENCE.md` is missing 9 shipped commands. | Moderate for docs, low for runtime. `help llm` gives the wrong search order. Four HT16K33 matrix settings show as read-only in the settings editor. There is no working gate. | Small (about 1-2 hours) | Fix before 1.0 in one small commit. |
| G-89 | USERGUIDE multi-hop section contradicts itself on pairing through relays | Yes. Only the line numbers have moved. | Low, docs only. A user may try to pair through a relay and the request never arrives. | Trivial (1-2 lines of docs) | Fix before 1.0 as part of the docs pass. |

### Fix before 1.0

- **G-88: command-registry gate and stale reference.**
  - **Evidence:**
    - `python3 tools/command_registry.py audit` exits 1 with 13 problems.
      - 9 are false positives on `ota*`. They come from two `otaCommands[]` tables in opposite `#if HW1_OTA_LAYOUT`/`#else` branches (`System_OTA.cpp:2169` and `:2191`).
      - 4 are real orphan cmdKeys: `matrixbrightness`, `matrixrotation`, `matrixsquare` and `matrixpanel` (`i2csensor_ht16k33.cpp:46-53`).
    - There is no `--check` mode (`tools/command_registry.py:439-448`).
    - The regenerated reference has 1006/1015 commands/entries; the tracked file says 997/1006. The missing commands are `memdiag`, `pdmamplify`, `pdmhpf`, `sttdetector`, `sttraw`, `sttdecisionpre`, `sttnoisepercentile`, `sttsilencefloor` and `sttspeechfloor` (from 89242716 and 44b085c1, not regenerated in c6d8dd21).
    - The llm overview at `System_CommandRegistry.cpp:859-860` gives the search order backwards. The code checks `/system/llm` first, then `/sd/llm`.
  - **Bounded fix:**
    1. Add `reference --check` (render, compare with `docs/COMMAND_REFERENCE.md`, exit 1 on a difference). Make the audit treat `otaCommands` as mutually exclusive branches and stop counting the dead branch.
    2. Add thin setter commands `matrixbrightness <0-15>`, `matrixrotation <0-3>`, `matrixsquare <0|1>` and `matrixpanel <0|1>` over the existing setSetting path.
    3. Change the llm overview to "internal /system/llm then /sd/llm".
    4. Regenerate `docs/COMMAND_REFERENCE.md`.
    5. Add a host-suite test that runs `audit` and `reference --check`, and add both commands to RELEASING.md "Gate: before any tag".
  - **Done when** `audit` exits 0, `reference --check` exits 0, and both run in the host suite.

- **G-89: USERGUIDE pairing-through-relays contradiction.**
  - **Evidence:**
    - `docs/USERGUIDE.md:572-573` says relays carry "the initial pairing handshake". `:591-592` says pairing is single-hop.
    - The code agrees with the second statement. `System_ESPNow.cpp:5324-5325` and `:5363` exclude `PAIR_*` from flooding and from `RELAY_DATA`, while KEY_EX and SESSION frames do relay. The `espnowmeshttl` help at `:13280` and `:18252` says "pairing [is] always single-hop".
  - **Bounded fix:**
    - Delete " — including the initial pairing handshake" at `:572-573`.
    - After the single-hop sentence at `:591-594`, add: "Once two devices share the mesh passphrase, their key exchange and encrypted session setup do travel through relays, so a paired peer that moves out of range stays reachable."
    - Optional: reword the "far-peer pairing" `static_assert` strings at `System_ESPNow_Wire.h:884-888` to "far-peer key exchange".

### Add to KNOWN_ISSUES

- **G-87: `openwifi` is synchronous and can stall other commands.**
  - **Evidence:**
    - Plain `openwifi` runs `connectToBestWifiNetwork()` with `kWifiConnectHuntBudgetMs = 150000` (`System_WiFi.h:159`, `:190`). Each network gets 35 s with 2 s gaps, plus inline NTP on connect.
    - `--index` and `--saved-id` take 35 s, plus a 15 s rollback (`System_WiFi.cpp:993-997`).
    - The web "save and connect" button (`WebPage_Settings.h:2825`) goes through `submitAndExecuteSync` with a 60 s wait on the single httpd task.
    - `G2_Page_Network.cpp:1245` and two host harnesses depend on the synchronous behaviour.
  - **KNOWN_ISSUES line:** "`openwifi` runs on the shared command executor. If saved networks are out of range or the password is wrong, other commands from every transport wait up to about 150 s (best-network hunt) or 50 s (`--index`/`--saved-id`). The web page reports a timeout after 60 s while the connect attempt continues. Use `wifistatus` to watch progress."
  - **Optional for 1.0:** add a cliHint to the `openwifi` help pointing to `wifistatus`.
  - **Post-1.0 bounded fix:**
    - Add an opt-in `openwifi async [--best|--index N|--saved-id id]`, modelled on `cameraAsyncForm`.
    - It runs the same connect code in a one-shot task with `BOOT_NET_STACK_WORDS`, refuses while a connect is already running, and replies at once.
    - Keep the synchronous default and switch only `WebPage_Settings.h:2825` to the async form.
    - Done when the async form returns in under 100 ms, a host suite covers supersede/rollback, and bench runs on feathers3 and p4x_eye are recorded.

### Close

These four are closed for 1.0 and move to the post-1.0 backlog. Each is still real, but none is a release blocker.

- **G-83: duplicate known-task tables.**
  - **Evidence:**
    - `System_TaskUtils.cpp:710-724` has 13 rows including `espnow_tx`. `System_MemoryMonitor.cpp:391-404` has 12 rows and no `espnow_tx`. Each has its own positional `taskAlive[]` array.
    - MemoryMonitor calls `uxTaskGetStackHighWaterMark(handle)` live (`:432-434`), which TaskUtils avoids on purpose (`:733-740`).
    - The `input_gamepad` comments and the System_Utils mention at `HAL_Input.h:61-63` are stale.
  - **Backlog bounded fix:**
    1. Add a single `collectKnownTaskRows()` provider in System_TaskUtils (a `KnownTaskRow` struct carrying name, handle, stack bytes, alive and snapshot fields).
    2. Have MemoryMonitor use it and read high-water marks from the snapshot. Drop the `taskIdx == 1` special case.
    3. Fix the comments.
    4. Add a host check that the row names match `xTaskCreate*` names.

- **G-84: command name pairs.**
  - **Evidence:**
    - 12 pairs, the latest being `c6console`/`c6_console` at `System_RadioCompanion.cpp:1233/1236`.
    - `docs/APP_JSON_CONTRACT.md:103/105` documents `blestatus`/`wifistatus` with their aliases.
    - Internal callers and the voice table use `srstart`, `srstop`, `closesr`, `blestatus`, `espnowstatus` and `wifistatus`.
    - The convention is the `*read`/`open*` form (21 and 23 reference rows).
  - **Backlog bounded fix:** a description-only pass that sets the non-canonical help text to "Alias of <canonical>.", then regenerates the reference and reruns the registry and overview tests. Correct the group's done-condition to "no duplicated `*read` rows".

- **G-85: System_Utils catch-all header.**
  - **Evidence:**
    - `System_Utils.h:54-56` includes WebServer_Server.h under `ENABLE_HTTP_SERVER`. About 86 of the 105 including files get it only this way.
    - The executor is still in `System_Utils.cpp:4066-~5000`.
    - `MODULARIZATION_PLAN.md:579-583` records that a networking-disabled header probe fails because of this.
  - **Backlog bounded fix:**
    1. Remove the WebServer_Server.h include and add direct includes where the compiler reports missing declarations.
    2. Check that feathers3 and p4x_eye link with HTTP both on and off.
    3. Move the executor to `System_CommandExecutor.cpp` in a separate commit, then run the command_task_isolation, register_submit, command_executor_admission and command_resolution_contract host suites.

- **G-86: compatibility-shim labels.**
  - **Evidence:**
    - `G2_SID_STATE_EVENT` (`System_G2_Protocol.h:118`) has no users. `formatMacAddrStr` has no callers, and `formatMacAddr` has one (`WebPage_Bond.cpp:982`).
    - `SCREEN_*` (58/42 hits), `OledMenuItem` (42), `getClientIp(String&)` (about 9 production callers) and `SR_TASK_PRIORITY_LEVEL` are live APIs labelled legacy.
  - **Backlog bounded fix:**
    1. Delete `G2_SID_STATE_EVENT`.
    2. Migrate the one `formatMacAddr` caller to `macToDisplay` and delete both aliases at `System_Utils.h:454-461`.
    3. Reword the labels at `HAL_Display.h:226-229` and `:268`, `OLED_Display.h:181`, `WebServer_Utils.cpp:234`, `System_TaskUtils.h:135` and `System_Debug_Manager.h:54` as supported aliases or convenience forms.
    4. Keep `ESPNOW_V4_FLAG_ENCRYPTED` as a reserved bit.
    - Verify with a feathers3 build.
