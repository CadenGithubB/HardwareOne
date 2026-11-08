# Appendix A: verified finding groups at 95dc3f34 (2026-11-06)

One skeptic per group re-opened the cited code at 95dc3f34 (read-only) and tried to refute it. Verdict vocabulary: confirmed, partly_confirmed, refuted, fixed_since_v0.99.99, cannot_determine. Original priority is the 2026-10-30 consolidator's.

| Group | Title | Was | Now | Verdict | Kind now |
|---|---|---|---|---|---|
| G-01 | Fresh clone cannot build: vendored Arduino core is git-ignored and unscripted | P0 | P0 | confirmed | confirmed_defect |
| G-04 | Secrets in public git history and personal identifiers tracked at HEAD | P0 | P0 | partly_confirmed | confirmed_defect |
| G-05 | Licence statements conflict and third-party notices are missing | P0 | P0 | confirmed | confirmed_defect |
| G-09 | OTA/recovery updater has no hardware qualification on shipping binaries; deployment OTA pipeline and key custody unexercised | P0 | P0 | confirmed | evidence_gap |
| G-10 | Documented account recovery (factoryreset) is unreachable without admin credentials | P0 | P0 | confirmed | confirmed_defect |
| G-12 | No tracked compatibility statement: companion versions, upgrade data-preservation and frozen surfaces | P0 | P0 | partly_confirmed | evidence_gap |
| G-02 | Uncommitted layer depends on an unrecorded BLE-library patch (getLastDisconnectReason) | P0 | P1 | partly_confirmed | confirmed_defect |
| G-03 | Bare idf.py path pairs the tracked XIAO sdkconfig with the feathers3 default board | P0 | P1 | confirmed | confirmed_defect |
| G-06 | 8 MB S3 image headroom is at the ceiling with no build record at the tag and no headroom gate | P0 | P1 | confirmed | evidence_gap |
| G-07 | Public board and hardware tables state facts the tracked code contradicts | P0 | P1 | partly_confirmed | confirmed_defect |
| G-08 | Documented plain install halts forever on a used board | P0 | P1 | confirmed | confirmed_defect |
| G-11 | Task watchdog never reboots on any non-OTA profile | P0 | P1 | confirmed | confirmed_defect |
| G-13 | handheld/p4x_eye deployment contract was never built and diverges from the bench-checked profile and release notes | P1 | P1 | confirmed | confirmed_defect |
| G-14 | No supported matrix with tiers; P4X-EYE and peripheral rows lack qualification status | P1 | P1 | confirmed | evidence_gap |
| G-16 | No CI, no clean-checkout build record and no RC/beta stage before a tag | P1 | P1 | confirmed | evidence_gap |
| G-17 | Native host suite is red at HEAD (three committed failures) | P1 | P1 | partly_confirmed | confirmed_defect |
| G-19 | Host-suite run is incomplete: Node/browser suites never run, OTA protocol suite unregistered, cloner-missing inputs, coverage unrecorded | P1 | P1 | confirmed | evidence_gap |
| G-21 | Uncommitted layer renames the user command c6console to c6_console, hand-edits COMMAND_REFERENCE.md and leaves CHANGELOG [Unreleased] empty | P1 | P1 | partly_confirmed | confirmed_defect |
| G-23 | Release-record drift: version declared in 7 places, 0.99.98.5 untagged, [Unreleased] empty despite a breaking event rename | P1 | P1 | confirmed | confirmed_defect |
| G-24 | tools/build_deployment.sh skips the Arduino patch guard and accepts a dirty tree | P1 | P1 | confirmed | confirmed_defect |
| G-25 | No executable proof that the tracked patch set reproduces the vendored Arduino tree | P1 | P1 | confirmed | evidence_gap |
| G-26 | P4X-EYE SDK preparation is unscripted and the 'clean' build uses a locally patched ESP-IDF | P1 | P1 | partly_confirmed | unverified_risk |
| G-28 | No SECURITY.md, private reporting channel, issue templates, known-issues, troubleshooting or diagnostics pages | P1 | P1 | confirmed | evidence_gap |
| G-29 | OTA/update documentation is false in six places and the user guide has no update/recovery section | P1 | P1 | confirmed | confirmed_defect |
| G-32 | Discover->confirm pairing control frames are single-shot and the state machine has no test or dated hardware record | P1 | P1 | confirmed | confirmed_defect |
| G-34 | BLE peers leaving range and returning have no dated hardware record; R1 wedge fix untested; phone reconnect scan never backs off | P1 | P1 | partly_confirmed | evidence_gap |
| G-38 | Corrupt settings.json or owner credential file is never quarantined: settings-read-only device, blocked factoryreset, silent owner lockout | P1 | P1 | confirmed | confirmed_defect |
| G-39 | Capped logs become uncapped when rotation cannot allocate; the LittleFS reserve only redirects; the SD overflow latch drops all logs after a card yank | P1 | P1 | confirmed | confirmed_defect |
| G-41 | No power-interruption or physical-media qualification for any atomic-save claim | P1 | P1 | confirmed | evidence_gap |
| G-42 | No dated soak or endurance record for any profile and no repeatable capture bundle | P1 | P1 | partly_confirmed | evidence_gap |
| G-43 | BLE open/close per-cycle leak verified fixed only on the P4; S3/classic cycle drift unmeasured | P1 | P1 | confirmed | evidence_gap |
| G-44 | Classic-board G2 admission floor admits a Wi-Fi+HTTP+G2 combination that lands near 9 KB free | P1 | P1 | confirmed | confirmed_defect |
| G-45 | Resident task stacks run with under 1.5 KB free with canary-only detection; c6_console raise uncommitted | P1 | P1 | confirmed | unverified_risk |
| G-47 | Portable/battery claim is unmeasured: no current or runtime figure, battery backends unvalidated, power caveats untracked | P1 | P1 | partly_confirmed | evidence_gap |
| G-48 | Clock owner skips IDF's external-memory speed sync at P4 100/200 MHz and S3 UltraSaver 40 MHz | P1 | P1 | confirmed | unverified_risk |
| G-50 | Stored XSS via filenames in the shared web file explorer (authenticated user-to-admin RCE through /api/cli) | P1 | P1 | confirmed | confirmed_defect |
| G-51 | Wi-Fi scan renders beacon SSIDs raw into innerHTML on the admin Settings page (zero-credential radio-range XSS) | P1 | P1 | confirmed | confirmed_defect |
| G-52 | First-time setup accepts a reserved owner name and boots into permanent lockout | P1 | P1 | confirmed | confirmed_defect |
| G-56 | 24 bad-argument replies return bare Usage: text and are classified as success by the OK:/Error: contract | P1 | P1 | confirmed | confirmed_defect |
| G-58 | QUICKSTART first-boot wizard table predates the 2026-10-10 regroup and Basic-mode advice names the wrong step | P1 | P1 | confirmed | confirmed_defect |
| G-59 | USERGUIDE documents commands that do not resolve, removed features, stale auto-pair wording and wrong privilege/hardware names | P1 | P1 | confirmed | confirmed_defect |
| G-60 | P4 local-STT profile needs a runtime model file no tracked artifact or URL provides | P1 | P1 | confirmed | evidence_gap |
| G-61 | Docs claim boards/xiao_s3.defaults turns Bluetooth off; presets turn Bluedroid on for every board | P1 | P1 | confirmed | confirmed_defect |
| G-15 | Shared-header defaults enable features every contract disables; on-device LLM compiled into classic-ESP32 images contrary to docs | P1 | P2 | confirmed | evidence_gap |
| G-18 | Three web-UI JavaScript tests fail at HEAD | P1 | P2 | partly_confirmed | confirmed_defect |
| G-20 | CMake source selection and System_BuildConfig.h guards are hand-mirrored for 17 feature families with one agreement test | P1 | P2 | confirmed | evidence_gap |
| G-22 | Tracked docs, CHANGELOG and source comments reference documents a cloner cannot read; the ignore policy behind it is hidden | P1 | P2 | partly_confirmed | confirmed_defect |
| G-27 | Images are not byte-reproducible and BUILD_INFO provenance is too thin to audit a release | P1 | P2 | confirmed | evidence_gap |
| G-30 | No identity or partition-layout check at cable-flash time; app-only flash over an older table corrupts the data partition | P1 | P2 | partly_confirmed | unverified_risk |
| G-31 | Backup covers a fixed subset with no NVS, notifications policy or capture key, while USERGUIDE calls it the device's configuration | P1 | P2 | confirmed | confirmed_defect |
| G-33 | G2 half-connected orphan never recovers at HEAD; the escalation exists only uncommitted and untested | P1 | P2 | fixed_since_v0.99.99 | unverified_risk |
| G-35 | ESP-NOW multi-hop relay and channel-follow have no host harness or hardware record | P1 | P2 | partly_confirmed | evidence_gap |
| G-36 | ESP-NOW file transfer radio path has no host run or hardware record | P1 | P2 | partly_confirmed | evidence_gap |
| G-37 | C6 companion outage handling has no induced-outage hardware record | P1 | P2 | partly_confirmed | evidence_gap |
| G-40 | Login salt is the device key: a flash-UID read failure or representation change locks out every account | P1 | P2 | confirmed | unverified_risk |
| G-46 | P4X-EYE composite internal-heap low-water unmeasured after the October tranches | P1 | P2 | partly_confirmed | evidence_gap |
| G-49 | Explicit light sleep is one OLED press away, ignores the API return value and has no hardware record | P1 | P2 | partly_confirmed | unverified_risk |
| G-53 | Wi-Fi driver persists the PSK in plaintext NVS on every default board | P1 | P2 | partly_confirmed | confirmed_defect |
| G-54 | No BLE link-layer security; G2/R1 peers trusted by saved address or advert-name pattern | P1 | P2 | partly_confirmed | unverified_risk |
| G-55 | Authorization gates lack executable tests for reserved-name sentinel, bonded FS-RPC and metadata scrub | P1 | P2 | partly_confirmed | evidence_gap |
| G-57 | Four disagreeing roster parsers and first-user fallbacks decide privilege | P1 | P2 | confirmed | optional_improvement |

## G-01 (P0, confirmed): Fresh clone cannot build: vendored Arduino core is git-ignored and unscripted

**Claim at 95dc3f34.** At 95dc3f34 components/arduino is still git-ignored and hard-required, no tracked script vendors or patches it, README/QUICKSTART still go from git clone straight to idf.py or build_board.sh, and tools/build_deployment.sh still calls idf.py directly without the build_board.sh preflight. The documented re-apply sequence also now omits one recorded patch (see G-02), so following it exactly still ends in a build_board.sh refusal.

**Why this priority.** No outside user can build any board from the public tree by following README or QUICKSTART, and the release script can build against an unpatched core without warning; that blocks a trustworthy 1.0 outright.

**Refuted or overstated.** The sub-point about the untracked variants/XIAO_ESP32S3_SENSE directory does not matter: no tracked board selects it (boards/xiao_s3.defaults uses XIAO_ESP32S3) and the 95dc3f34 checkout's vendored copy has no such directory. ORCH-1's own 2026-10-30 run shows the manual procedure worked at aa6508fd, so this is a missing script plus wrong docs, not a design defect.

**Evidence.**
- `.gitignore:99` components/arduino/ still ignored; git check-ignore confirms
- `components/hardwareone/CMakeLists.txt:469` arduino is still in the hardwareone requirement list
- `tools/build_board.sh:84` Lines 84-97: preflight unchanged since aa6508fd; refuses when components/arduino is missing ('Vendor arduino-esp32 3.3.5 there...', no URL or commit) or verify_patches.sh fails
- `README.md:201` Lines 197-209: clone then `idf.py -p PORT flash monitor`, no vendoring step; file unchanged since aa6508fd
- `README.md:229` Still says the patches are needed only for G2/R1 builds and calls it 'the managed Arduino BLE component'
- `docs/QUICKSTART.md:79` 'The firmware vendors arduino-esp32 3.3.5'; clone at line 90 is followed by no provisioning step; file unchanged since aa6508fd
- `tools/build_deployment.sh:132` Main release image built with plain idf.py (lines 123/132/141); only check is idf.py on PATH (63-64); no arduino or verify_patches reference anywhere in the 217-line script
- `docs/arduino-local-patches/README.md:54` Lines 54-62: re-apply sequence still the only procedure; it lists snapshot + 5 dated patches + tools/ble-phone-pipe/arduino.patch and omits the 2026-11-06 connect-semaphores patch that d5f7b9ac added
- ... 2 more in verify/A1.json

**Newer commits.** d5f7b9ac: added a dated patch file and re-pinned the phone-pipe patch but did not add the new file to the README re-apply sequence or to verify_patches.sh's failure hint; 3a3e3ba0..fc4a0cdf (Waveshare series): a new esp32p4 board (boards/waveshare_p4_touch_lcd_5.defaults) that needs the same vendored core and P4 hosted hunks

**Waveshare.** Adds one more board (waveshare_p4_touch_lcd_5, esp32p4) that cannot be built from a fresh clone; like p4x_eye it also needs tools/p4/prepare_sdk.sh. It does not change the fix.

**Bounded fix.** Add a tracked tools/vendor_arduino.sh: clone espressif/arduino-esp32 at 11bc7ac1a458f1f4e7afc8fd4de1dfa710f31562 into components/arduino, apply the snapshot, the six dated patches (including 2026-11-06) and tools/ble-phone-pipe/arduino.patch in order, then run verify_patches.sh and check_sdk.py. Make it step 1 in README and QUICKSTART, have build_board.sh print its name, run the same preflight in build_deployment.sh, and fix README.md:229 and docs/arduino-local-patches/README.md:3-9 and 91-97.

**Done when.** In an empty directory with only ESP-IDF 5.5.5 exported: git clone of the release commit, tools/vendor_arduino.sh, then tools/build_board.sh feathers3 exits 0 with BUILD_INFO 'clean tree'; verify_patches.sh and check_sdk.py exit 0; tools/build_deployment.sh exits non-zero when components/arduino is absent or a stock 3.3.5 copy.


## G-04 (P0, partly_confirmed): Secrets in public git history and personal identifiers tracked at HEAD

**Claim at 95dc3f34.** The serial capture components/hardwareone/nocaseseriallogsespnowtest.txt (317,423 bytes, 16 lines matching wifiadd/wificonnect/useradd/login shapes, values not printed) is in root commit a8014ca4, which is still the only root of HEAD and an ancestor of origin/main (4ed1eaa5, fetched 2026-11-06), 6 other remote branches and every tag from v0.9 to v0.99.99; tag v0.99.92 still carries 186 docs including docs/AUTH_SECURITY_REVIEW.md. HEAD no longer tracks the file. HEAD still tracks the operator's macOS account home path in 5 docs (2 of them new Waveshare docs), the home LAN subnet in 4 files, and the home SSID in 3 docs. Every MAC-shaped string at HEAD is a 02:48:57:31:0X:YY placeholder or an obvious test value.

**Why this priority.** A home Wi-Fi passphrase and an admin password one `git show` away in a public repo's history, reachable from origin/main and all 57 release tags, is the one item a trustworthy public 1.0 cannot ship with; it needs owner action, not code.

**Refuted or overstated.** Device MACs at HEAD: refuted. Every MAC-shaped string in tracked docs, experiments and tests is the documented 02:48:57:31:0X:YY placeholder scheme or an obvious test value (AA:BB:CC.., 00:11:22..). No real vendor OUI remains, and 4ed1eaa5 finished that job. The serial-port names are short location-based usbmodemNNNN names, low sensitivity. 'HEAD still tracks the capture file' is wrong: only history does. The audit-doc count is 36 by my pattern, against the 30 originally reported.

**Evidence.**
- `components/hardwareone/nocaseseriallogsespnowtest.txt @ a8014ca4:1` Blob still resolves (git cat-file -s = 317423). Deleted only at 9ef87c32, so it is still in history. `git rev-list --max-parents=0 HEAD` = a8014ca4. `git merge-base --is-ancestor a8014ca4 origin/main` is true.
- `git branch -r --contains a8014ca4:None` origin/main, origin/codex/android-ble-relay, origin/codex/investigate-esp32-p4x-eye, origin/codex/jpeg-portable, origin/codex/p4x-eye-tools-cleanup, origin/program-fixes-merged, p4-local/stt-speedups; 11 local branches.
- `git tag --contains a8014ca4:None` All 57 tags, v0.9 through v0.99.99 including v0.99.98.3/.4. Whether each tag is on GitHub needs `git ls-remote --tags origin` (network, not run).
- `git ls-tree -r v0.99.92 docs:None` 186 docs, 36 of them matching AUDIT|SECURITY|HARDENING|_REVIEW (docs/AUTH_SECURITY_REVIEW.md blob present). v0.99.99 and HEAD carry only docs/investigations/2026-09-27-p4x/code-audit.md, a benign P4 portability appendix.
- `.gitignore:37` Lines 37-58 record the leak and ignore *seriallog*, the audit docs and docs/purge_plan.sh. The ignore rules only stop it happening again. docs/purge_plan.sh is not in this checkout and no purge has run.
- `docs/STT_FOOD_AUDIO_INVESTIGATION_20261011.md:7` Named-account home path at lines 7, 11, 19, 26, 36, 41, 80.
- `docs/OPENCLAW_SETUP.md:49` Home LAN subnet at 49, 136, 138, 155, 169, 358; home SSID at 35, 127, 136, 358; home path at 398, 410; USB serial port at 129, 145, 406, 413. The MACs at 130 and 407 are now placeholders.
- `docs/G2_WEAR_DETECTION_23214.md:46` Home path at 46 and 129.
- ... 6 more in verify/C.json

**Newer commits.** 4ed1eaa5: replaced the remaining real device addresses with placeholders (docs only); left the SSID, LAN IPs and home paths in place; 3a3e3ba0..b2387608 (Waveshare series): added docs/WAVESHARE_P4_SUPPORT.md and WAVESHARE_P4_INTEGRATION.md with new home-path, SSID and LAN-IP occurrences

**Waveshare.** Adds identifiers: the two new Waveshare docs carry the operator's home path, SSID and LAN IP (WAVESHARE_P4_SUPPORT.md:30, 653, 655, 701; WAVESHARE_P4_INTEGRATION.md:18, 99).

**Bounded fix.** Owner: rotate the Wi-Fi PSK and the admin password, then run a reviewed git-filter-repo purge of the capture file (all refs) and the audit docs from the v0.99.92 tree, re-create the tags and releases, force-push, and ask GitHub to GC. Repo: replace the home paths, LAN IPs and SSID in the 8 files above with placeholders (or move the session notes to the ignored area), set config.example.json bind_host to 0.0.0.0, drop the v0.99.92 pointer in OTA_DEVICE_SETUP.md, and add a grep step for '/Users/[a-z]', the 192.168.21. subnet and the SSID to RELEASING.md.

**Done when.** On a fresh clone: `git log --all -- components/hardwareone/nocaseseriallogsespnowtest.txt` prints nothing; `git cat-file -e a8014ca4` fails; `git ls-tree v0.99.92 -- docs/AUTH_SECURITY_REVIEW.md` prints nothing; `git grep -nIE '/Users/[a-z]|192\.168\.21\.|<private>' -- . ':!components/hardwareone_libs'` prints nothing; the owner has recorded the rotation date privately.


## G-05 (P0, confirmed): Licence statements conflict and third-party notices are missing

**Claim at 95dc3f34.** LICENSE.md is PolyForm Noncommercial 1.0.0 while docs/USERGUIDE.md:1905-1907 still says 'MIT License'. No NOTICE or THIRD_PARTY file is tracked. WebPage_DarkRoom.h embeds MPL-2.0 A Dark Room but points at third_party/adarkroom/LICENSE.md, which is git-ignored. LGPL-3 Adafruit_NeoPixel is compiled in (hardwareone_libs/CMakeLists.txt:9, System_NeoPixel.cpp:28). The LGPL-2.1 arduino-esp32 core is git-ignored, so its licence is absent from a clone.

**Why this priority.** A first public release that states two different licences and ships MPL/LGPL code without the required licence text undermines trust in every other claim; the text fixes are small, though the LGPL-under-noncommercial question needs an owner decision.

**Evidence.**
- `LICENSE.md:1` PolyForm Noncommercial License 1.0.0.
- `docs/USERGUIDE.md:1907` 'MIT License' under '## License' (1905).
- `components/hardwareone/WebPage_DarkRoom.h:6` 'License: Mozilla Public License 2.0 (see third_party/adarkroom/LICENSE.md)'. `git ls-files third_party` is empty and .gitignore:103 ignores the directory.
- `components/hardwareone_libs/CMakeLists.txt:9` Adafruit_NeoPixel in SRC_DIRS (also INCLUDE_DIRS line 29). Used at components/hardwareone/System_NeoPixel.cpp:28,33.
- `.gitignore:99` components/arduino/ is ignored, so its LGPL-2.1 LICENSE.md exists on disk but not in a clone (`git ls-files components/arduino` = 0).
- `components/hardwareone/idf_component.yml:46` espressif/esp-sr (Espressif-MIT, ESPRESSIF-products-only grant) is a dependency.
- `components/hardwareone/third_party/es8311/LICENSE:1` NEW Waveshare-era Apache-2.0 driver with its licence file tracked. It belongs in a notices list.
- `README.md:None` No licence or third-party statement (`git grep -i licen -- README.md` is empty).

**Newer commits.** fa3473c2: added components/hardwareone/third_party/es8311 (Apache-2.0, licence included) for the Waveshare speaker; one more entry for a notices file

**Waveshare.** Adds one third-party component (es8311, Apache-2.0, licence file present). This does not change the defect.

**Bounded fix.** Change USERGUIDE.md:1905-1907 to point at LICENSE.md. Add NOTICE.md listing each compiled third-party component (the hardwareone_libs SRC_DIRS, managed_components, arduino-esp32 with its LGPL statement, es8311, A Dark Room) with licence and path. Track the MPL-2.0 text next to WebPage_DarkRoom.h. Record the owner's decision on statically linking the LGPL code (or swap NeoPixel for IDF led_strip).

**Done when.** `git grep -n 'MIT License' docs/USERGUIDE.md` is empty; NOTICE.md is tracked and names every SRC_DIRS entry, every idf_component.yml dependency, es8311 and A Dark Room; `git ls-files | grep -i mpl` or NOTICE.md contains the MPL-2.0 text; the LGPL decision is written down.


## G-09 (P0, confirmed): OTA/recovery updater has no hardware qualification on shipping binaries; deployment OTA pipeline and key custody unexercised

**Claim at 95dc3f34.** At 95dc3f34 the only dated OTA hardware record in tracked docs is the FeatherS3 run of 2026-08-05 on v0.99.8; CHANGELOG 0.99.98 lists OTA recovery as not exercised; the updater last changed 2026-10-13 with no hardware run; the three OTA deployments and the QT Py OTA layout have no recorded migration or signed apply; the qualification CLI is dry-run only; RELEASING.md has no OTA key/signing/bundle procedure.

**Why this priority.** README advertises signed recovery OTA as a feature, and the shipping updater, layouts and deployment pipeline have no hardware record since August; the one path that cannot be fixed remotely must be qualified or labelled experimental before 1.0.

**Refuted or overstated.** 'No executable OTA test since 2026-08' is overstated: the main application's journal policy and probation supervisor have host harnesses. The build/deployments BUILD_INFO files cited are untracked artifacts of another checkout and are not present at the 95dc3f34 tree; they are not evidence either way.

**Evidence.**
- `docs/OTA_DEVICE_SETUP.md:6` 'Procedure verified end to end on a plain FeatherS3 on 2026-08-05 ... on firmware v0.99.8' - still the only dated OTA hardware claim; no OTA/recovery entry in docs/investigations, experiments/board_qualification/RESULTS.md, WAVESHARE_P4_SUPPORT.md or any post-aa6508fd record (grep).
- `CHANGELOG.md:858` 0.99.98 verification scope: 'Not exercised on hardware: ... factory reset, OTA recovery'. [Unreleased] (line 11) is empty.
- `CHANGELOG.md:681` 0.99.98 updater semantics change (downgrade/forced-power not carried); 'takes an updater from this release in the factory slot'.
- `updater/main/updater_main.c:1434` boot_reconcile; apply_staged at 893. git log updater/: last change ed9a6a9f 2026-10-13, nothing since aa6508fd.
- `updater/test/host/test_recovery_auth_throttle.c:35` Only updater host test. Main-side OTA policy does have host harnesses (components/hardwareone/test/host/ota_journal_policy_harness.cpp, ota_probation_supervisor_harness.cpp), but updater apply/resume/reconcile has none.
- `tools/ota/README.md:18` 'The current qualification CLI intentionally has no destructive scenario executor'; STG-012..018 in tools/ota/qualification/scenarios.py are contracts only.
- `boards/qtpy_esp32.ota.defaults:15` QT Py revision argued from the Feather V2 reading on 2026-08-25 ('same die family'); no QT Py OTA hardware record.
- `deployments/pocket_assistant/boards/xiao_s3/MIGRATION.md:65` 'Confirm the recovery updater is reachable before relying on OTA'; line 66 cites 'ota status' (the main-app command is otastatus).
- ... 3 more in verify/B.json

**Newer commits.** none of the 47 commits since aa6508fd touch updater/, tools/ota or components/hw1_ota_protocol; System_OTA.cpp changed only cosmetically

**Waveshare.** No change to OTA scope: the Waveshare board is factory-only with no OTA contract (partitions_no_sr_32mb_waveshare_p4_touch_lcd_5.csv:2).

**Bounded fix.** Either (a) on the RC's own updater+main pair, on one FeatherS3 and one 8 MB board, run migration-flash, recovery console, SoftAP upload, one signed staged apply with probation and otaack, one forced rollback to recovery, and one interrupted write with resume, and record dated PASS lines; or (b) relabel recovery OTA 'experimental, not qualified for 1.0' in README:139 and the Headless Node section, and add an OTA key-custody paragraph to RELEASING.md.

**Done when.** The 1.0 CHANGELOG entry carries per-board dated PASS lines for the listed OTA cases on the tagged artifacts (with the check_ota_builds.py OK line), or README marks every OTA layout without such a line as experimental; RELEASING.md names the signing-key location policy and build_deployment.sh invocation.

**Hardware needed.** FeatherS3 and Feather V2 (or QT Py): migration-flash, recovery console unlock, SoftAP upload, signed staged apply + probation + otaack, forced probation failure with bootloader rollback into recovery, power cut during ota_0 write then resume; XIAO pocket_assistant migration if that deployment stays advertised.


## G-10 (P0, confirmed): Documented account recovery (factoryreset) is unreachable without admin credentials

**Claim at 95dc3f34.** USERGUIDE 'Account recovery' still calls factoryreset the way back in after lost admin credentials. factoryreset is registered requiresSuperAdmin=true, and serial requires login by default, so a single-owner device that has lost its only super-admin password can only recover with a cable erase. No boot-hold or marker re-arm exists.

**Why this priority.** The user guide promises a recovery path that a locked-out owner cannot use. Correcting the text and documenting a LittleFS-only erase is cheap and needed for a trustworthy 1.0. A physical re-arm can follow.

**Refuted or overstated.** Minor overstatement. A device with a second admin account can recover through userresetpassword, and a device whose owner turned serialRequireAuth off has an AuthBypass serial session. The lockout as claimed applies to the default single-owner case. Also, the USERGUIDE line says accounts are the wipe's only target, but factoryreset also deletes every account's /stt transcripts and notes (help text at System_Utils.cpp:2784-2788), which is a second inaccuracy in the same paragraph.

**Evidence.**
- `docs/USERGUIDE.md:431` 'Account recovery' (431-435) still says factoryreset is the intended way back after lost admin credentials; the command table at :988 says '(admin)', but the command actually needs a super-admin
- `components/hardwareone/System_Utils.cpp:2782` factoryreset registry row: requiresAdmin=true and requiresSuperAdmin=true at :2789
- `components/hardwareone/HardwareOne.cpp:3015` serial auth gate: with serialRequireAuth set, any command without a login gets 'Authentication required' (:3021)
- `components/hardwareone/System_Settings.h:386` serialRequireAuth defaults to true; the AuthBypass path (System_User.cpp:611) works only if the owner turned it off earlier
- `components/hardwareone/System_FirstTimeSetup.cpp:118` setup re-arms only when users.json is absent (:121-122); no button, boot-window or marker re-arm
- `components/hardwareone/System_User.cpp:4782` userresetpassword (admin) lets a second admin reset another account, so only a device with a single admin is fully locked out
- `components/hardwareone/System_Utils.cpp:2470` factoryreset also stops when settings.json is unparseable (pipe-secret persist refused), so the documented path can fail even for a logged-in super-admin (see G-38)
- `docs/OTA_DEVICE_SETUP.md:110` the only documented reset is esptool erase_flash (:110, :116); no doc mentions erase_region of the littlefs partition

**Newer commits.** none: System_FirstTimeSetup.cpp changed only for touch input (oledWizardButtonPresses), and USERGUIDE changed only in the events table

**Waveshare.** Same behaviour. The Waveshare touch UI reaches the same login and setup flow, and recovery would use its own 32 MB partition table (littlefs at 0x810000, size 0x17F0000), so any documented erase_region procedure needs per-layout offsets.

**Bounded fix.** Rewrite USERGUIDE 'Account recovery' (and the :988 row) to say a super-admin login is required, that a second admin can use userresetpassword, and that factoryreset also deletes transcripts and notes. Add a cable procedure that erases only the littlefs region per partitions/*.csv, and say that it also loses settings and captures. Optionally add one physical re-arm that renames users.json to users.json.bad and reuses PENDING_CRED_SETUP_FILE.

**Done when.** USERGUIDE no longer says factoryreset works without credentials, and it names a procedure that was tested once on hardware. If a re-arm is added, a host runner shows roster present -> re-arm -> SETUP_REQUIRED.

**Hardware needed.** One run of the documented littlefs-only erase_region on a locked-out board (for example a FeatherS3) that reaches the setup wizard with the firmware intact.


## G-12 (P0, partly_confirmed): No tracked compatibility statement: companion versions, upgrade data-preservation and frozen surfaces

**Claim at 95dc3f34.** No tracked COMPATIBILITY document or upgrade/data-preservation statement exists at 95dc3f34. Companion version gates (C6 exact 2.12.13, four exact R1 strings, G2 profile pairs, CM5 protocol 1, phone-link v2) live only in code or scattered CHANGELOG/tool READMEs. settings.json has only a logged firmwareVersion stamp and no schema or migration. A renamed automation event (g2_worn -> g2_plugin_alive, committed in 64713c0d) now silently never fires for saved automations, with no alias or note.

**Why this priority.** Calling a release 1.0 means stating what is promised. Without a short compatibility and upgrade statement, a public user cannot tell which C6, app, ring, glasses or Pi versions work, or whether an update keeps their data, and some failures (renamed automation events) are silent.

**Refuted or overstated.** The untracked Android doc is now tracked and labelled historical (64713c0d). The 'phone app too old' failure is now partly documented in tools/phone_link_peer/README.md:35. The remaining claim stands.

**Evidence.**
- `components/esp_now_hosted/include/esp_now_hosted_companion.h:18` HW1_C6_HOSTED_TAG v2.12.13, BLE max 4, flasher v1.10.0; HAL_Radio.cpp:292-294 and 399-401 require exact major/minor/patch equality
- `components/hardwareone/System_R1_Protocol.cpp:130` R1 admitted only on exact deviceInfo strings (130-142); not stated in README/USERGUIDE/CHANGELOG
- `components/hardwareone/System_Cm5HostControl.cpp:28` kProtocolVersion "1"
- `tools/phone_link_peer/README.md:35` Says a v1 app (2.7.x) never reaches READY with a v2 P4. No minimum compatible app version is named anywhere
- `docs/ANDROID_AGENT_PROMPT_CONSOLE_ON_PHONE_LINK.md:1` Now tracked (64713c0d) and marked historical, pointing to the phone-link v2 peer guide, so the dangling-reference part is fixed
- `components/hardwareone/System_Settings.cpp:1956` 1956-1972: version mismatch only logged and posted as SYSEVT_FIRMWARE_CHANGED; no schema integer or migration
- `components/hardwareone/System_EventCatalogRows.h:61` g2_plugin_alive replaces g2_worn (committed 64713c0d); git grep finds no g2_worn alias in firmware sources
- `components/hardwareone/System_Automation.cpp:2517` Unknown trigger 'on' names parse as evtKind 0 and never match. Line 1677 skips them when the automation is re-serialized, so saved g2_worn automations silently stop firing
- ... 3 more in verify/A2.json

**Newer commits.** 64713c0d: tracked the Android prompt doc; committed the g2_worn -> g2_plugin_alive rename and edited the event fixture with no CHANGELOG line; ab387b02: legacy C6 companions limited to explicit c6update sessions (another compatibility rule a user must know); 3e5d4579: Waveshare legacy C6 upgrade support

**Waveshare.** Adds one compatibility row. A Waveshare board ships with legacy C6 Hosted firmware that has no version RPC. Normal radio use refuses it until the user runs c6update with the pinned 2.12.13 companion image. This appears only in docs/WAVESHARE_P4_*.md, not in user docs.

**Bounded fix.** Add a tracked docs/COMPATIBILITY.md linked from README and RELEASING.md. Give one row per companion: C6 2.12.13 exact (stock or Waveshare factory firmware needs c6update), minimum phone-link v2 app build, CM5 protocol 1, G2 profile versions, R1 exact strings. Add an upgrade paragraph, for example '1.0.0 is a full-erase install; within 1.x settings and automations are kept, and renamed keys or events get an alias or a CHANGELOG Changed line'. Add a g2_worn alias in systemEventKindFromName or a CHANGELOG note now.

**Done when.** git ls-files docs/COMPATIBILITY.md lists it; README and RELEASING.md link it; every version literal in it matches its source literal (manually or by a grep test); systemEventKindFromName("g2_worn") > 0, or CHANGELOG states the rename under Changed.


## G-02 (P1, partly_confirmed): Uncommitted layer depends on an unrecorded BLE-library patch (getLastDisconnectReason)

**Claim at 95dc3f34.** getLastDisconnectReason is now recorded: d5f7b9ac put it in tools/ble-phone-pipe/arduino.patch (marker 2026-11-04), re-pinned all six manifest files, raised the verify floor to 16, and added symbol checks; the vendored copy matches every manifest 'after' hash. The documented re-apply sequence and the verify_patches.sh hint still omit the new 2026-11-06 connect-semaphores patch, so the documented sequence gives 15 BLEClient.cpp markers (verify fails) and a BLEClient.cpp that does not match the manifest. Of the two harness breakages, oled_layout was fixed in 19b3d09a; wifi_connect_budget is most likely still red.

**Why this priority.** The headline defect (an unrecorded BLEClient patch) is fixed. What remains is a wrong re-apply list that fails loudly at the build preflight, plus one stale host harness, so it is real but not a 1.0 blocker on its own.

**Refuted or overstated.** 'A cloner replaying the recorded patch series cannot build any ENABLE_G2_GLASSES profile' and 'phone_sdk_manifest is red on the bench' no longer hold: the accessor is in the tracked arduino.patch and the bench copy matches the manifest. 'wt_only' no longer applies: the layer is committed (64713c0d).

**Evidence.**
- `tools/ble-phone-pipe/arduino.patch:154` BLEClient.cpp hunk records m_lastDisconnectReason on a matched DISCONNECT; BLEClient.h hunk (lines 160-175) adds getLastDisconnectReason()
- `tools/ble-phone-pipe/manifest.json:20` BLEClient.cpp and BLEClient.h re-pinned in d5f7b9ac; sha256 of all six vendored files equals 'after', and arduino.patch equals patch_sha256 (computed read-only at 95dc3f34)
- `docs/arduino-local-patches/verify_patches.sh:39` BLEClient.cpp floor is 16 (vendored copy has 17); lines 69-70 add symbol checks for the accessor and the assignment
- `docs/arduino-local-patches/README.md:54` Lines 54-62 re-apply sequence omits arduino-local-patches-2026-11-06-ble-connect-semaphores.patch (listed only in the table at line 34)
- `docs/arduino-local-patches/verify_patches.sh:127` Lines 127-133 failure hint lists dated patches only up to 2026-10-25; it names neither the 2026-11-06 patch nor tools/ble-phone-pipe/arduino.patch
- `docs/investigations/2026-11-04-p4x-fix-progress.md:1005` Lines 1005-1014: arduino.patch and the manifest were regenerated against a baseline that already includes the 2026-11-06 hunks, so the order matters
- `components/hardwareone/G2_Glasses.cpp:3930` Caller of getLastDisconnectReason(); also G2_Ring.cpp:5404
- `components/hardwareone/test/host/test_oled_layout.py:35` 19b3d09a now injects TASK_CPU_SAMPLE_MS from System_TaskUtils.h:476 when OLED_Mode_System.cpp:252 aliases it, which fixes the ORCH-3 compile error
- ... 1 more in verify/A1.json

**Newer commits.** 64713c0d: committed the layer that calls getLastDisconnectReason (1 caller) without recording the patch; the tree could not be reproduced from 2026-11-01 until d5f7b9ac; d5f7b9ac: recorded the disconnect reason in arduino.patch, added the 2026-11-06 dated patch, re-pinned the manifest, raised the floor to 16; did not update the README sequence or the verify hint; 19b3d09a: fixed the oled_layout harness extraction; a9d47ea3: second caller (G2_Ring) of getLastDisconnectReason

**Waveshare.** c1d1dbac (Waveshare glasses and phone links) touched System_WiFi.cpp. It does not change the patch situation; the Waveshare G2/phone profiles need the same arduino.patch.

**Bounded fix.** Add the 2026-11-06 patch line before tools/ble-phone-pipe/arduino.patch in docs/arduino-local-patches/README.md:54-62, and add both files to the verify_patches.sh failure hint. Drop the unused SYSEVT_WIFI_CONNECTED constant from wifi_connect_budget_harness.cpp, or mark it [[maybe_unused]].

**Done when.** Replaying the README sequence onto a pristine 11bc7ac1a copy gives verify_patches.sh exit 0 and check_sdk.py exit 0; ctest -R 'phone_sdk_manifest|oled_layout|wifi_connect_budget|ble_client_connect_semaphores' passes at the release commit.


## G-03 (P1, confirmed): Bare idf.py path pairs the tracked XIAO sdkconfig with the feathers3 default board

**Claim at 95dc3f34.** Unchanged at 95dc3f34. With no HW_BOARD, a bare idf.py uses the tracked config/sdkconfig (XIAO_ESP32S3 variant, octal PSRAM, 8MB flash) but defaults the board to feathers3 and picks the 16 MB table partitions_no_sr_16mb.csv. Board reconcile is skipped, so the saved 8MB wins and gen_esp32part should reject the table. The same configure also rewrites the tracked config/sdkconfig to clear its 'partitions.csv' path, which dirties the tree. Docs still give two default-board rules.

**Why this priority.** README's first build command fails at configure or build rather than producing a bad image, and the per-board command beside it works, so a docs fix plus an optional CMake refusal closes it; it is not a silent hazard.

**Refuted or overstated.** SCOPE-1's 'or produces a mis-targeted image' branch is unlikely: the flash-size check should stop the build first. The original agents did not run it either; it is inferred from code.

**Evidence.**
- `README.md:205` `idf.py -p PORT flash monitor` straight after clone (unchanged since tag)
- `CMakeLists.txt:42` Lines 42-46: bare idf.py uses config/sdkconfig
- `CMakeLists.txt:130` Lines 130-135: esp32s3 with no HW_BOARD defaults to feathers3; lines 173-177 take flash=16mb from the board file; lines 481-483 pick partitions_no_sr_16mb.csv
- `CMakeLists.txt:531` Saved values win over defaults; lines 546-559 file(WRITE) the tracked config/sdkconfig to clear the stale partition path
- `CMakeLists.txt:693` Lines 693-701: hw1_reconcile_sdkconfig runs only with ENV HW_BOARD, a feature config, or a deployment
- `config/sdkconfig:557` CONFIG_ESPTOOLPY_FLASHSIZE="8MB"; 575 CUSTOM_FILENAME="partitions.csv"; 583 ARDUINO_VARIANT="XIAO_ESP32S3"; 1559 SPIRAM_MODE_OCT=y
- `partitions/partitions_no_sr_16mb.csv:35` littlefs ends at 0x625000+0x9DB000 = 16 MB
- `/Users/<private>/esp/esp-idf/components/partition_table/gen_esp32part.py:372` verify_size_fits raises 'does not fit in configured flash size'; partition_table/CMakeLists.txt:43-44 passes CONFIG_ESPTOOLPY_FLASHSIZE
- ... 2 more in verify/A1.json

**Newer commits.** f688bcc1: only touched CMakeLists.txt to add NETWORK_EVENTS_MUTEX; default-board, reconcile and partition logic unchanged since aa6508fd

**Bounded fix.** Remove the bare idf.py lines from README.md:204-205 and QUICKSTART.md:129-143 so tools/build_board.sh is the only documented entry, and fix BOARD_SWITCHING.md:415/460. Optionally make CMake fail fast when HW_BOARD is defaulted and the saved CONFIG_ARDUINO_VARIANT or flash size disagrees with the board file.

**Done when.** grep finds no bare-idf.py first-build instruction in README/QUICKSTART and exactly one default-board rule matching CMakeLists.txt:130-135; in a clean clone a bare `idf.py reconfigure` either completes self-consistently or stops with a message naming build_board.sh, and git status stays clean.


## G-06 (P1, confirmed): 8 MB S3 image headroom is at the ceiling with no build record at the tag and no headroom gate

**Claim at 95dc3f34.** The gap is still open and has grown. The 95dc3f34 checkout has no xiao_s3 or feathers3 BUILD_INFO at all; the last S3 figures remain 3,456 B (0.1%) for bare xiao_s3 at f694a01b+76 (2026-10-26) and 73,536 B (1.2%) for feathers3 in ORCH-1's aa6508fd worktree. Since then ~5,300 lines of shared component code have landed, including ~380 net in G2/Ring/BLE code that the S3 Bluetooth builds compile. No headroom gate exists and CHANGELOG has no entry since the tag.

**Why this priority.** README's primary-board command may not link and FeatherS3 is near its ceiling, but an overflow fails the build loudly and one build per board settles it. It becomes P0 only if that build overflows.

**Refuted or overstated.** Nothing refuted. Note that the camera viewer added in 19b3d09a compiles only with ENABLE_OLED_DISPLAY && ENABLE_CAMERA_SENSOR, so it does not grow the bare xiao_s3 lane (display off) or feathers3 (no camera).

**Evidence.**
- `build-config-p4-phone-pipe/BUILD_INFO.md:None` Newest BUILD_INFO in the checkout: built 2026-11-06 08:38 from a33742bc (clean tree), P4X 16MB, 1,280,512 B free (17.2%); not an S3 lane
- `build-config-p4-openclaw/BUILD_INFO.md:None` 2026-11-05 22:11, 9674eb49 + 53 uncommitted, 18.6% free (P4)
- `build-config-p4-phone-pipe-heapdiag/BUILD_INFO.md:None` 2026-11-05 21:06, 9674eb49 + 43 uncommitted, 17.3% free (P4)
- `build-config-waveshare-p4-bringup/BUILD_INFO.md:None` 2026-11-01 12:17, 82919267 + 71 uncommitted, 70.8% free (Waveshare 32MB)
- `README.md:208` `tools/build_board.sh xiao_s3` quick-start; line 131 'Primary dev target'; unchanged since tag
- `boards/xiao_s3.features.h:8` Board profile still turns off only MQTT, I2C, display, input; Bluetooth/G2/LLM/maps inherit; unchanged since tag
- `configurations/README.md:27` Bluetooth-off is still a saved-profile choice (xiao-default), not the board default
- `tools/gen_build_info.py:435` Reports headroom only (also 548); no floor. tools/build_board.sh has no size gate
- ... 2 more in verify/A1.json

**Newer commits.** 64713c0d: 18.7k-line layer including G2_Glasses changes (S3 Bluetooth builds compile it); a9d47ea3, d0f050f9, 37b2fa23, fbc6fecc: more G2/Ring/phone-pipe code in the S3 Bluetooth image; 3a3e3ba0..fc4a0cdf: Waveshare flags added to System_BuildConfig.h default off for S3 (ENABLE_SPEAKER 0, GT911 by input type)

**Waveshare.** The Waveshare lane has 70.8% headroom (32MB flash), so it adds no size risk itself. Its shared-header additions default off on S3.

**Bounded fix.** From a clean checkout of the release candidate, build tools/build_board.sh xiao_s3 and feathers3 and record both BUILD_INFO headroom figures. Then either point README.md:131/208 at `tools/build_config.py build xiao-default` and mark bare xiao_s3 unsupported, or slim boards/xiao_s3.features.h. Add a headroom floor (e.g. 64 KB) to gen_build_info.py.

**Done when.** BUILD_INFO.md for xiao_s3 and feathers3 from the tagged revision (clean tree) are recorded in the CHANGELOG release entry with headroom at or above the stated floor, or README names the saved profile that meets it; a build below the floor exits non-zero.

**Hardware needed.** One boot of each recorded image (FeatherS3: boot, OLED/gamepad, Wi-Fi, web login, G2 connect; XIAO: boot, Wi-Fi, camera) settles the 'no dated device check' part.


## G-07 (P1, partly_confirmed): Public board and hardware tables state facts the tracked code contradicts

**Claim at 95dc3f34.** At 95dc3f34 README still lists AMG8833, TEA5767 and a 'QT PY ESP32-S3' with no code, says the XIAO has no OTA layout, and QUICKSTART/USERGUIDE/BOARD_SWITCHING still misstate XIAO Bluetooth, FeatherS3 flash size, the default board, and omit p4x_eye, feathers3_fe and the new Waveshare board plus DISPLAY_TYPE 4/5, INPUT_DEVICE_TYPE 3/4 and ENABLE_LLM_SOURCE_HTTP. The g2_worn event row is fixed.

**Why this priority.** The public README/QUICKSTART/USERGUIDE list hardware with no driver and misstate board facts, which erodes trust and misdirects purchases, but nothing is damaged and one doc pass fixes it.

**Refuted or overstated.** The g2_worn/g2_not_worn event-row defect is fixed at 95dc3f34 (64713c0d). The qtpy_esp32 half of the QUICKSTART Bluetooth sentence is true (its preset sets CONFIG_BT_ENABLED=n). README's P4 '32 MB' PSRAM figure is unverified, not shown false. Original P0 overstated: this is misleading documentation, not a device or data hazard.

**Evidence.**
- `README.md:100` 'MLX90640 / AMG8833 thermal camera'; also line 153 AMG8833 purchase row. git grep -i amg88 over components/hardwareone returns nothing; only the vendored unreferenced Adafruit_AMG88xx library mentions it.
- `README.md:107` 'TEA5767 FM Radio receiver'; also line 158 purchase row. No TEA5767 in components/hardwareone; the driver is i2csensor_rda5807.cpp (the vendored Radio library merely supports TEA5767).
- `README.md:139` 'The XIAO ESP32-S3 has no OTA layout yet' while deployments/pocket_assistant/boards/xiao_s3/contract.conf:7-8 sets OTA_LAYOUT=1, hw1-pa-xiao-ota-v1.
- `README.md:166` Camera and PDM mic rows (166-167) name 'QT PY ESP32-S3'; no board file, no pin block, string exists only here. P4X-EYE and Waveshare (both have camera/mic) absent from the rows.
- `docs/QUICKSTART.md:48` Lines 48-50 and table 177 + callout 181-187: xiao_s3 'Bluetooth off'; presets/sdkconfig.defaults:9 sets CONFIG_BT_ENABLED=y for every board and only presets/qtpy_esp32.sdkconfig.defaults:3 sets =n (qtpy half of the claim is true).
- `docs/QUICKSTART.md:174` Four-board table; no p4x_eye, waveshare_p4_touch_lcd_5 or feathers3_fe row (boards/ now has seven real board files).
- `docs/USERGUIDE.md:29` 'All feature flags live in one file ... No other files need to change' - contradicted by boards/*.features.h, deployments/*/features.h and configurations/*.json (CONTRIBUTING.md ownership table).
- `docs/USERGUIDE.md:42` DISPLAY_TYPE 0-3 and INPUT_DEVICE_TYPE 0-2 (42-43); System_BuildConfig.h:523-524 defines 4 (P4X-EYE ST7789) and 5 (HX8394 Waveshare); 529-530 define INPUT 3 (GPIO encoder) and 4 (GT911 touch).
- ... 5 more in verify/B.json

**Newer commits.** 64713c0d: committed the USERGUIDE g2_plugin_alive/silent row fix; 3a3e3ba0: added boards/waveshare_p4_touch_lcd_5 (DISPLAY_TYPE 5, INPUT 4), which none of the four board docs mention

**Waveshare.** Adds to it: a seventh board file, DISPLAY_TYPE 5 and INPUT_DEVICE_TYPE 4 exist only in docs/WAVESHARE_P4_SUPPORT.md and configurations/README.md; README, QUICKSTART, USERGUIDE and BOARD_SWITCHING do not name the board at all.

**Bounded fix.** One documentation pass: delete or mark 'not implemented' the AMG8833, TEA5767 and QT PY ESP32-S3 rows; reword README:139 to name pocket_assistant/xiao_s3; fix QUICKSTART BT sentence/table to the presets rule; fix FeatherS3 flash size and default board in BOARD_SWITCHING; add p4x_eye, feathers3_fe, waveshare rows and DISPLAY_TYPE 4/5, INPUT 3/4, ENABLE_LLM_SOURCE_HTTP to USERGUIDE/QUICKSTART. A generator script is optional, not required for 1.0.

**Done when.** git grep -n -i 'AMG8833\|TEA5767\|QT PY ESP32-S3\|no OTA layout yet\|XIAO ESP32S3 Plus' -- README.md docs/ returns nothing; every non-.ota basename in boards/*.defaults appears in the QUICKSTART and USERGUIDE board tables; the enum values listed in USERGUIDE equal System_BuildConfig.h:519-530; grep -c ENABLE_LLM_SOURCE_HTTP docs/USERGUIDE.md >= 1.


## G-08 (P1, confirmed): Documented plain install halts forever on a used board

**Claim at 95dc3f34.** QUICKSTART's install (tools/build_board.sh <board> -p PORT flash monitor) never writes the LittleFS partition and never mentions erase or littlefs-flash; on a board whose data region is non-blank and not a mountable LittleFS at the current offset, initFilesystem() refuses to format and setup halts in 'FATAL: Filesystem initialization failed' with no remedy printed (non-OTA builds have no recovery fallback).

**Why this priority.** A first-time user with a previously used board reaches a halting 'FATAL' with no next step from the primary documented install, which is recoverable without data loss but very visible; a doc line and a better message close it.

**Refuted or overstated.** Minor inaccuracies: the bytes at the new S3 offset are old LittleFS blocks, not 'old factory-app bytes' (the old LittleFS started below the new offset); the outcome is the same. The P4 case (pre-2026-10-01 table) applies only to bench devices: the P4 board and its own CSV entered the public tree together in v0.99.96 (5a1065e8), so no tagged P4 release used the old table. A fully erased board is handled (blank region auto-formats). Original P0 overstated: the board is not bricked and no data is lost.

**Evidence.**
- `docs/QUICKSTART.md:102` Section 3 flash step, then 'That's it.' (line 148); no erase or littlefs-flash in README.md, QUICKSTART, USERGUIDE or BOARD_SWITCHING (grep).
- `components/hardwareone/System_Filesystem.cpp:129` LittleFS.begin(false,...); formats only if littlefsPartitionIsBlank (142), otherwise 'LittleFS mount failed; preserving data' (157) and returns false.
- `components/hardwareone/HardwareOne.cpp:1690` if (!initFilesystem()) prints 'FATAL: Filesystem initialization failed', calls otaSystemRecoverFromStorageFailure() and loops forever; no remedy named.
- `components/hardwareone/System_OTA.cpp:1944` otaSystemRecoverFromStorageFailure returns false when !HW1_OTA_LAYOUT (1965-1967), so ordinary board builds just halt.
- `CMakeLists.txt:863` littlefs_create_partition_image(littlefs ...) without FLASH_IN_PROJECT: 'idf.py flash' does not write littlefs.bin; the remedy target littlefs-flash exists on every board (905) and the comment at 848-857 acknowledges the halt.
- `docs/OTA_DEVICE_SETUP.md:273` Only documented use of littlefs-flash, inside the OTA provisioning guide.
- `partitions/partitions_no_sr_16mb.csv:25` LittleFS offset moved on 2026-08-16; boards last flashed with an older table hold old LittleFS blocks (non-0xFF) at the new offset.

**Waveshare.** Adds a case: partitions_no_sr_32mb_waveshare_p4_touch_lcd_5.csv:1-2 says the layout is not compatible with the vendor demo layout and first deployment needs a full layout migration; a Waveshare board fresh from the vendor demo will hit the same halt unless erased, and the Waveshare docs only record that the bench region was erased with approval.

**Bounded fix.** Add a 'If the board stops at FATAL: Filesystem initialization failed' step to QUICKSTART section 3 and README quick-build (tools/build_board.sh <board> -p PORT littlefs-flash, or erase-flash with the stated NVS loss), and make the HardwareOne.cpp FATAL line print that command on non-OTA builds.

**Done when.** grep -n 'littlefs-flash' docs/QUICKSTART.md README.md matches; the FATAL string in HardwareOne.cpp names littlefs-flash; one bench run on an S3 board holding non-blank data at the LittleFS offset shows the new message and reaches first-time setup after the documented command.

**Hardware needed.** Flash the RC with the documented command onto an S3 board (and a Waveshare with its vendor demo, if it stays advertised) whose LittleFS region holds non-LittleFS data; confirm the halt, run littlefs-flash, confirm first-time setup.


## G-11 (P1, confirmed): Task watchdog never reboots on any non-OTA profile

**Claim at 95dc3f34.** Only config/sdkconfig.ota.defaults sets CONFIG_ESP_TASK_WDT_PANIC=y. config/sdkconfig.defaults, the esp32/esp32s3/esp32p4 chip files, every boards/*.defaults (including the new waveshare_p4_touch_lcd_5 and the *.ota overlays) and presets/ leave it unset. The application subscribes no task (no esp_task_wdt_add in components/hardwareone or main/), so on every non-OTA image a TWDT trip only prints and a deadlock that leaves IDLE running goes undetected.

**Why this priority.** A wedged device needs a power cycle, which matters for headless use, but nothing is lost. Turning on PANIC needs a false-positive soak first (known IDLE-starvation paths exist), so a P0 one-line flip would be risky.

**Refuted or overstated.** Nothing substantive. The claim that PANIC appears nowhere in the defaults chain still holds after checking every chain and boards/*.defaults, including the new Waveshare file and the qtpy/feather OTA overlays. P0 is overstated, because the fix risks false-positive reboots until it has been soaked.

**Evidence.**
- `CMakeLists.txt:526` defaults chain: config/sdkconfig.defaults(+.<target>) -> boards/<board>.defaults -> partition defaults -> [OTA only: config/sdkconfig.ota.defaults, boards/<board>.ota.defaults] -> presets/policy files (:526-585)
- `config/sdkconfig.ota.defaults:22` TASK_WDT_EN/INIT/PANIC=y (22-24), the only place PANIC is set in the firmware chain; updater/sdkconfig.defaults:30 sets it for the separate recovery updater only
- `config/sdkconfig.defaults:102` sets only CONFIG_ESP_INT_WDT_TIMEOUT_MS=1500 (:105); no TASK_WDT keys in any config/sdkconfig.defaults*, boards/*.defaults or presets/* (grep)
- `config/sdkconfig:1677` '# CONFIG_ESP_TASK_WDT_PANIC is not set'; TIMEOUT_S=5, only the IDLE CPU0/1 checks
- `build-config-waveshare-p4-bringup/sdkconfig:2030` PANIC not set on the new Waveshare profile; the same holds for build-config-p4-phone-pipe/sdkconfig:2020, p4-openclaw and p4-phone-pipe-heapdiag (local build dirs)
- `components/hardwareone/HardwareOne.cpp:1621` comment (1621-1627) still says non-OTA layouts 'only print and keep running' on a trip
- `components/hardwareone/System_OTA.cpp:995` the only esp_task_wdt_* call in the app (a reset); no esp_task_wdt_add in components/hardwareone or main/hardwareone-idf.cpp. The SleepyDog library has one but no caller uses it
- `components/hardwareone/System_OTASafety.cpp:44` the 30 s loop-hang reboot supervisor runs only for an unverified OTA image, so it does not cover normal boots
- ... 2 more in verify/D1.json

**Newer commits.** 3a3e3ba0/4b3c0550: add boards/waveshare_p4_touch_lcd_5.defaults without TWDT keys (another non-PANIC profile); ddf94817: status now labels later reset reasons; TASK_WDT reporting exists (HardwareOne.cpp:1316)

**Waveshare.** Adds one more affected profile. The Waveshare defaults set no TWDT keys and the bring-up build has PANIC unset. The board has no OTA layout, so no Waveshare image gets PANIC today.

**Bounded fix.** Add CONFIG_ESP_TASK_WDT_PANIC=y to config/sdkconfig.defaults (optionally TIMEOUT_S=10). Subscribe main (esp_task_wdt_add(NULL) in setup, reset in loopHealthTick) and cmd_exec (reset per dequeued command). Add an admin-only wdttest command.

**Done when.** Every saved board profile's sdkconfig has CONFIG_ESP_TASK_WDT_PANIC=y, and grep shows esp_task_wdt_add for main and cmd_exec. wdttest reboots one S3 and one classic board, and status reports a TASK_WDT reset. A 2 h idle+web+glasses soak on a FeatherS3 and a P4 records zero TWDT trips.

**Hardware needed.** wdttest reboot on one S3 and one ESP32 board, and a 2 h false-positive soak on a FeatherS3 and a P4X-EYE or Waveshare with glasses or BLE active.


## G-13 (P1, confirmed): handheld/p4x_eye deployment contract was never built and diverges from the bench-checked profile and release notes

**Claim at 95dc3f34.** deployments/handheld/boards/p4x_eye is unchanged since v0.99.99. It still sets ENABLE_AUTOMATION 0, against README:94 (Standard Handheld = automations) and CHANGELOG:31. It also omits the board profile's ENABLE_STT_SPEAKER_LABELS 1. No handheld build record or contract test exists. The new Waveshare P4 board has no deployment contract at all.

**Why this priority.** The one reproducible P4 release contradicts the README matrix and the v0.99.99 notes, and has never been built. That breaks trust in the documented release path but does not harm devices.

**Refuted or overstated.** 'SD off, ESP-SR on' is not a divergence from the board profile: boards/p4x_eye.features.h also has SDMMC 0 and ESP_SR 1. Those differ only from the p4-phone-pipe saved configuration. The 'no build/deployments directory' observation was made on another machine and cannot be re-checked in this checkout (there is no build/ here), but no record of a handheld build exists in the tracked tree either.

**Evidence.**
- `deployments/handheld/boards/p4x_eye/features.h:121` #define ENABLE_AUTOMATION 0; line 21 lists automations as excluded
- `boards/p4x_eye.features.h:104` Board profile has ENABLE_STT_SPEAKER_LABELS 1 and no AUTOMATION override (inherits System_BuildConfig.h:371 = 1). Diff of #defines vs contract: real deltas are only AUTOMATION and SPEAKER_LABELS. SD_CARD and QUICKNOTE resolve the same
- `README.md:94` Automations row: Standard Handheld column ticked; README:48 names deployments/handheld as the P4X-EYE's reproducible profile
- `CHANGELOG.md:31` 'The P4X-EYE build includes automations' (true only for the board-profile build)
- `tools/ota/tests/test_deployment_contract.py:43` Tests load only the two headless contracts; no handheld or OTA_LAYOUT=0 case in tools/ota/tests
- `CHANGELOG.md:1320` Only mention of `build_deployment.sh handheld p4x_eye` is where it was introduced; no dated build or boot of it in CHANGELOG, docs or docs/investigations
- `deployments/handheld/README.md:9` Board table lists only the P4X-EYE
- `configurations/waveshare-p4-bringup.json:3` Waveshare saved profile also sets ENABLE_AUTOMATION 0
- ... 1 more in verify/A2.json

**Newer commits.** 3a3e3ba0 / 1d653608: Waveshare P4 board family added as a board profile + saved configuration only; no deployments/*/boards/waveshare_p4_touch_lcd_5 contract

**Waveshare.** The Waveshare ESP32-P4-WIFI6-Touch-LCD-5 has no deployment contract. It is a board profile (boards/waveshare_p4_touch_lcd_5.*), a board-specific factory-only 32 MB partition table and the saved configuration waveshare-p4-bringup (automations off as well). It is absent from README, QUICKSTART, USERGUIDE and CHANGELOG. It would fit as a second factory-only handheld contract, deployments/handheld/boards/waveshare_p4_touch_lcd_5 with FLASH_SIZE=32MB and OTA_LAYOUT=0, derived from the saved profile. Otherwise 1.0 should list it as unsupported or experimental. Either way the automation decision should be made once for both P4 handhelds.

**Bounded fix.** Decide automations for the handheld family. Then either drop features.h:121-122 (and the line-21 exclusion) after a size-gated build, or amend README:94 and CHANGELOG to say the handheld release excludes automations. Run `tools/build_deployment.sh handheld p4x_eye` once at the release commit and record it. Add one OTA_LAYOUT=0 load test to tools/ota/tests. State Waveshare's 1.0 status (contract or experimental).

**Done when.** `grep 'define ENABLE_AUTOMATION' deployments/handheld/boards/p4x_eye/features.h` agrees with README:94; a CHANGELOG/release record names the handheld BUILD_INFO commit = tag and image size <= 0x705000; tools/ota/tests has a passing factory-only contract case; README states Waveshare's status.

**Hardware needed.** One boot of the handheld release image on a P4X-EYE, with automation add/list/delete if they are enabled.


## G-14 (P1, confirmed): No supported matrix with tiers; P4X-EYE and peripheral rows lack qualification status

**Claim at 95dc3f34.** There is still no single supported-matrix table with tier, update path and last dated hardware check; README's six product configurations map unevenly onto deployments and saved profiles, the peripheral tables carry no status, the P4X-EYE remains candidate/cable-only with SD, battery-only and other features unqualified, and the new Waveshare board is absent from README entirely.

**Why this priority.** A 1.0 user cannot tell which board/profile combinations are qualified, and the two P4 boards carry many explicitly unqualified features; a tiered matrix is the minimum honest scope statement.

**Refuted or overstated.** Not refuted. Dated hardware records have grown since aa6508fd (docs/investigations/2026-11-0x, WAVESHARE_P4_SUPPORT.md), which gives the matrix source material, but none of it is surfaced as tiers in README.

**Evidence.**
- `README.md:33` Six configurations (33-67); only Headless Node and Standard Handheld link a deployment. Sensor Appliance, Bonded, Wearable Companion and Pi pair link no contract or profile.
- `README.md:79` Feature table has a 'Bonded' column while every deployment sets ENABLE_BONDED_MODE 0 (deployments/headless/features.h:107, handheld/boards/p4x_eye/features.h:81, pocket_assistant/boards/xiao_s3/features.h:165).
- `README.md:129` Board table has no tier/qualification column and no Waveshare row; README never mentions the Waveshare board.
- `docs/BOARD_SWITCHING.md:130` Lists 'Generic ESP32' as a board; reachable only through the ARDUINO_ESP32_DEV fallback at System_BuildConfig.h:1371-1374.
- `docs/P4X_EYE_PERIPHERALS.md:274` 'Not yet qualified: the microSD card ..., battery-only operation and power draw, idle/sleep clocks, C6 crash recovery, and long endurance' plus two known issues.
- `docs/P4X_EYE_PERIPHERALS.md:172` Says microSD 'is off until it has been qualified' (board default ENABLE_SDMMC_CARD 0 at boards/p4x_eye.features.h:46-47) while configurations/README.md:36-38 keeps SDMMC enabled in every P4 speech profile and System_BuildConfig.h:1453-1454 requires ENABLE_SD_CARD for the 15x5 model.
- `configurations/README.md:40` p4-glasses-test 'has not been reflashed or requalified since' 2026-10-18; voice-to-agent path 'still needs hardware qualification'.
- `docs/investigations/2026-11-04-p4x-acceptance.md:64` Post-aa6508fd dated P4X BLE acceptance records exist; a later ring outage 'remains unqualified'.
- ... 1 more in verify/B.json

**Newer commits.** 3a3e3ba0..b2387608: Waveshare board bring-up and dated acceptance records, not reflected in README; 582f2c98 / d0f050f9: second P4X Bluetooth investigation and acceptance notes

**Waveshare.** Adds a seventh board that needs its own row (candidate, factory-only cable install, profile waveshare-p4-bringup, dated records in docs/WAVESHARE_P4_SUPPORT.md with many features still unqualified).

**Bounded fix.** Add one README 'Supported matrix' table: rows = board x (deployment | saved profile), columns = tier (stable / cable-only / candidate / experimental), update path, last dated hardware record link; mark Bonded, Generic ESP32 and the Waveshare explicitly; give peripheral rows a status column drawn from existing records.

**Done when.** README contains the matrix with a row for every non-.ota boards/*.defaults file and every deployments/*/boards/* contract; each row's 'last check' links a dated tracked record or says 'none'; P4X_EYE_PERIPHERALS.md:172/274 and configurations/README.md:36-38 agree on the SD status.


## G-16 (P1, confirmed): No CI, no clean-checkout build record and no RC/beta stage before a tag

**Claim at 95dc3f34.** There is no CI configuration, no release-candidate or go/no-go checklist, and no docs/releases record. RELEASING.md (unchanged since v0.99.99) is still commit, tag and notes only. The tree is clean at 95dc3f34 because 64713c0d absorbed the previously uncommitted layer. Nothing prevents tagging from a dirty tree, and no BUILD_INFO for a tagged commit is recorded.

**Why this priority.** A 1.0 tag needs at least one recorded clean-checkout build and install of the exact tagged commit per supported release. Today nothing in the tree shows a tag builds from tracked files alone. A minimal RC record is cheap and is what makes the tag trustworthy.

**Refuted or overstated.** The 'dirty tree' evidence was about the v0.99.99 cut. At 95dc3f34 the tree is clean (64713c0d committed the layer), so the defect is the missing gate and record, not a currently dirty tree.

**Evidence.**
- `RELEASING.md:28` 'Cut a release' steps 1-7: no build, host-suite, clean-checkout, install or size step; 'This repo's specifics' says releases are source-only
- `RELEASING.md:59` Still says 50 tags / 24 Releases
- `.gitignore:1` git ls-files has no .github/, .gitlab-ci.yml, Jenkinsfile, .circleci or ci script
- `CHANGELOG.md:11` [Unreleased] empty after 47 commits; no build matrix for 0.99.99
- `docs/investigations/2026-11-04-p4x-acceptance.md:109` Newer bench records are per-candidate image hashes from working builds, explicitly 'not proof' of release gates
- `docs/WAVESHARE_P4_INTEGRATION.md:54` Waveshare 'four full firmware builds passed' recorded from a working checkout, not a clean tag worktree

**Newer commits.** 64713c0d: absorbed the large uncommitted layer, so the tree is now clean; no CI or RC step added

**Waveshare.** Adds a sixth board, and a third P4 build lane, to any RC matrix. It has no contract, so an RC would rely on build_config.py waveshare-p4-bringup.

**Bounded fix.** Add a RELEASING.md 'release candidate' step: from a detached worktree of the rc tag, build each deployment contract and supported board, require BUILD_INFO commit = tag with dirty=0, run the host suite and tools/ota tests, flash each contract once by its MIGRATION.md, and write the table to docs/releases/<ver>.md. CI is optional.

**Done when.** docs/releases/1.0.0-rc.N.md exists listing per-profile BUILD_INFO commits equal to the tag (dirty=0), host-suite result, and one dated install per contract; RELEASING.md references the step.

**Hardware needed.** One documented-procedure install and short soak per release contract board (FeatherS3, Feather V2, XIAO S3, P4X-EYE; Waveshare if in scope).


## G-17 (P1, partly_confirmed): Native host suite is red at HEAD (three committed failures)

**Claim at 95dc3f34.** Of the three failures at aa6508fd, oled_mode_availability was updated in 19b3d09a to the new Bluetooth behaviour and should pass. tof_poll_outcomes (harness and test unchanged since 2026-10-09) and tool_pins (pins unchanged, fixture moved further in 64713c0d) should still fail. RELEASING.md still has no host-suite step. These are static inferences; the orchestrator's ctest run decides.

**Why this priority.** A red host suite cannot serve as the release gate. The remaining failures are stale harnesses, not shown product defects, so the fix is small.

**Refuted or overstated.** oled_mode_availability is fixed since v0.99.99 (19b3d09a, test-side).

**Evidence.**
- `components/hardwareone/test/host/oled_mode_availability_harness.cpp:259` 19b3d09a changed the R5 expectation to AVAILABLE when no BLE role runs; matches production OLED_Utils.cpp:6055 ('Always enterable')
- `components/hardwareone/test/host/tof_poll_outcomes_harness.cpp:86` Declares only SYSEVT_SENSOR_FAULT and SYSEVT_TOF_OBJECT_DETECTED; last changed 5047a24c (2026-10-09)
- `components/hardwareone/System_I2C_Manager.cpp:915` announceUnresponsive (915) and SYSEVT_SENSOR_RECOVERED (932) are still referenced by the extracted recordSuccess/recordError (test_tof_poll_outcomes.py:35-45) and are neither extracted nor declared
- `tools/ble_secure_event_catalog_client.py:51` Pins EXPECTED_CATALOG_BYTES 3149 / FAMILY 13 / KIND 165 (51-53); tools/ble_secure/test_secure_channel_v1.py:425,575 pin (13,165,3149)
- `components/hardwareone/test/host/fixtures/event_catalog_v1.json:None` Computed at 95dc3f34: 13 families / 174 kinds / 3307 compact bytes (changed in 64713c0d), so test_tool_pins.py still fails
- `RELEASING.md:54` Unchanged since tag; no ctest or host-suite step anywhere in the file

**Newer commits.** 19b3d09a: updated oled_mode_availability harness/test (Bluetooth AVAILABLE while off; camera rows); 64713c0d: grew the event catalog fixture to 174/3307 without re-pinning the client or protocol test; a9d47ea3: edited System_EventCatalogRows.h comments only

**Bounded fix.** In one commit: declare SYSEVT_SENSOR_RECOVERED and extract or stub announceUnresponsive in the ToF harness; re-pin tools/ble_secure_event_catalog_client.py and test_secure_channel_v1.py to 13/174/3307; add 'host suite green, with Node' to RELEASING.md.

**Done when.** ctest in components/hardwareone/test/host at the release commit reports 0 failures for oled_mode_availability, tof_poll_outcomes and tool_pins, and RELEASING.md lists the step.


## G-19 (P1, confirmed): Host-suite run is incomplete: Node/browser suites never run, OTA protocol suite unregistered, cloner-missing inputs, coverage unrecorded

**Claim at 95dc3f34.** Unchanged at 95dc3f34. Browser suites are DISABLED without Node, openclaw_terminal is not registered without Node, and several suites can skip with code 77. hw1_ota_protocol/test/host is still a standalone CMake project the main host CMakeLists never registers. build_coverage.sh and RELEASING.md are unchanged. The host README still does not list the vendored components/arduino as a prerequisite.

**Why this priority.** The OTA rollback and corruption guarantees are pinned only by a suite no runner executes, and the browser suites silently report as disabled. Wiring them into the standard run is cheap and needed for a trustworthy 'host suite green' claim.

**Refuted or overstated.** BLD-8's point about the app-filter CMake/C++ agreement is a documented, deliberate deferral (CONTRIBUTING.md 'Other source-selection rules...') and not a 1.0 issue; treat it as optional.

**Evidence.**
- `components/hardwareone/test/host/CMakeLists.txt:1307` Lines 1307-1336: find_program node; browser suites set DISABLED TRUE without it; 1435-1442 openclaw_terminal only a STATUS message
- `components/hardwareone/test/host/CMakeLists.txt:486` SKIP_RETURN_CODE 77 on espnow_page_render, also stt_lm_parity (1099), stt_conv_scratch (2158), camera_managed_sccb (2207), camera_video_buffers (2211)
- `components/hardwareone/test/host/CMakeLists.txt:2074` Only a comment mentions hw1_ota_protocol; no add_subdirectory or add_test for its suite
- `components/hw1_ota_protocol/test/host/CMakeLists.txt:1` Standalone project; no commits touched components/hw1_ota_protocol since aa6508fd
- `components/hardwareone/test/host/README.md:600` Points at the protocol suite without running it; arduino prerequisite mentioned only for one suite (line 1607), while test_command_header_boundary.py:23 and others read components/arduino directly
- `tools/build_coverage.sh:24` Unchanged since aa6508fd; no recorded run in CHANGELOG

**Newer commits.** 5a366504, 19b3d09a, fc4a0cdf, 64713c0d, 1d653608: edited the host README and CMakeLists (new suites, two new skip-77 camera suites) without registering the OTA protocol suite or the Node prerequisite

**Waveshare.** fc4a0cdf added camera_managed_sccb and camera_video_buffers with SKIP_RETURN_CODE 77, so a green run can hide two more skipped suites.

**Bounded fix.** Register components/hw1_ota_protocol/test/host from the main host CMakeLists, or run all host projects from one tools/run_host_suites.sh. Make configure fail when Node is absent unless browser suites are explicitly opted out. List the vendored-arduino prerequisite at the top of the host README. Record skipped/disabled counts in the release entry.

**Done when.** From the main host build, `ctest -R hw1_ota_protocol` runs and passes; the release ctest log shows 0 'Not Run' and names every skipped suite with its reason.


## G-21 (P1, partly_confirmed): Uncommitted layer renames the user command c6console to c6_console, hand-edits COMMAND_REFERENCE.md and leaves CHANGELOG [Unreleased] empty

**Claim at 95dc3f34.** The task-naming sweep, now committed in 64713c0d, renamed the user command c6console to c6_console. It is the only top-level registry command with an underscore, the companion README and the shipped CHANGELOG entry still teach c6console, and CHANGELOG [Unreleased] is empty although the rename, the powerdim/displayDimLevel removal, the g2_worn to g2_plugin_alive/silent event rename and the Waveshare P4 board family have all landed since v0.99.99. The hand-edited COMMAND_REFERENCE part is no longer true: the committed reference matches a fresh regeneration byte for byte.

**Why this priority.** A silent, undocumented rename of a shipped command plus an empty [Unreleased] section over 47 commits (a whole new board family included) would make the 1.0 release notes untrustworthy; both fixes are minutes of work.

**Refuted or overstated.** The claim that COMMAND_REFERENCE.md was hand-edited no longer holds: at 95dc3f34 it equals a fresh regeneration (LOCAL_WAVESHARE_MERGE.md records the regeneration). The claim is also no longer about an uncommitted layer: the rename is committed in 64713c0d, though it is not yet in any tag.

**Evidence.**
- `components/hardwareone/System_RadioCompanion.cpp:1233` Registry row { "c6_console", ... cmd_c6console }; the sibling rows keep the c6status/c6hold/c6update style. At aa6508fd this row (line 1232) was "c6console".
- `components/hardwareone/System_RadioCompanion.cpp:1066` Usage text now reads 'Usage: c6_console [on|off]' (also a bare Usage: reply, see G-56).
- `components/hardwareone/HAL_Radio.cpp:320` FreeRTOS task name c6_console. This was the intended target of the sweep (docs/TASK_INVENTORY_AND_CONSOLIDATION.md:51 lists only task names).
- `docs/LOCAL_WAVESHARE_MERGE.md:23` The merge record keeps 'the local c6_console task name' and says nothing of a command rename, so the command change looks accidental.
- `tools/p4/companion/README.md:47` Still documents `c6console`, which now returns Unknown command.
- `CHANGELOG.md:1149` Shipped name `c6console [on|off]`.
- `CHANGELOG.md:11` '## [Unreleased]' is followed straight away by '## [0.99.99] - 2026-10-28' (line 13). git describe gives v0.99.99-47-g95dc3f34, so there are 47 commits with no entry.
- `docs/COMMAND_REFERENCE.md:277` Lists `c6_console`. The header (line 7) reads 996 commands / 1005 entries. Running cmd_reference() in-process into the scratchpad gave output identical to the committed file, so it is no longer hand-edited.
- ... 3 more in verify/E2.json

**Newer commits.** 64713c0d: committed the c6_console command rename, the powerdim removal and the g2 event rename without CHANGELOG text; 1d653608: Waveshare merge kept the c6_console task name and regenerated the reference

**Waveshare.** The Waveshare P4 also runs the C6 companion over SDIO, so c6_console is registered there too. Without RADIO_C6_UART_TX_PIN, radioHalConsoleMirror(true) returns ESP_ERR_NOT_SUPPORTED (HAL_Radio.cpp:335-337). The Waveshare family itself is another major [Unreleased] item with no CHANGELOG text.

**Bounded fix.** Restore "c6console" in System_RadioCompanion.cpp:1233 and :1066, and in docs/P4X_EYE_PERIPHERALS.md:112. Keep the task name. Regenerate COMMAND_REFERENCE.md. Write [Unreleased] covering the Waveshare family, the task renames, the powerdim removal, the g2_worn to g2_plugin_* rename, crash records and log rotation. Optionally add a ^[a-z0-9]+$ (camelCase allow-list) check for root command names to command_registry.py audit.

**Done when.** git grep -n 'c6_console' -- components/hardwareone/System_RadioCompanion.cpp docs/COMMAND_REFERENCE.md docs/P4X_EYE_PERIPHERALS.md is empty. A regeneration of COMMAND_REFERENCE.md leaves git diff empty. The CHANGELOG section above [0.99.99] is non-empty and names powerdim and g2_plugin_*.


## G-23 (P1, confirmed): Release-record drift: version declared in 7 places, 0.99.98.5 untagged, [Unreleased] empty despite a breaking event rename

**Claim at 95dc3f34.** The version literal 0.99.99 still lives in seven places (CMakeLists 803/805, README:9, USERGUIDE:1, QUICKSTART:1, configurations/README.md:28, phone_console_harness.cpp:109/473) against the four RELEASING.md names. There is still no v0.99.98.5 tag. CHANGELOG [Unreleased] is empty although 47 commits since v0.99.99 add the Waveshare board, system-log rotation, crash records and the committed g2_worn -> g2_plugin_alive rename. Images built from 95dc3f34 report 0.99.99.

**Why this priority.** 47 commits of user-visible change, including a new board and an event rename that silently disables saved automations, have no CHANGELOG line and still identify as 0.99.99. A trustworthy 1.0 needs its release record to match the code, and the fix is cheap.

**Refuted or overstated.** The event rename is no longer 'uncommitted'; it landed in 64713c0d. That makes the missing CHANGELOG line worse, not better.

**Evidence.**
- `CMakeLists.txt:803` PROJECT_VER "0.99.99+suffix" / 805 "0.99.99"
- `components/hardwareone/test/host/phone_console_harness.cpp:109` Hard-coded "0.99.99 \"host\""; asserted at 473
- `configurations/README.md:28` 'for the 0.99.99 release'
- `RELEASING.md:15` Names four version sites
- `CHANGELOG.md:11` [Unreleased] empty; git diff aa6508fd..95dc3f34 -- CHANGELOG.md is empty
- `CHANGELOG.md:55` [0.99.98.5] entry; `git tag -l v0.99.98.5` empty; f694a01b is the v0.99.98.5 commit
- `docs/USERGUIDE.md:634` Event table already documents g2_plugin_alive/g2_plugin_silent (64713c0d), but CHANGELOG does not mention the rename
- `components/hardwareone/System_Automation.cpp:2517` Old names now parse as evtKind 0 (never match)

**Newer commits.** 64713c0d: committed the event rename and USERGUIDE row without a CHANGELOG entry; 3a3e3ba0..95dc3f34: 47 commits, none touching CHANGELOG.md or the version sites

**Waveshare.** Waveshare support is itself an unrecorded user-visible addition: there is no [Unreleased] line and no README board entry.

**Bounded fix.** Fill [Unreleased] now (Waveshare board, event rename with migration note, log rotation, crash records, BLE fixes). Add tools/check_release.sh asserting the version sites agree and that no stale previous-version literal remains outside CHANGELOG. Make the harness compare a prefix or read PROJECT_VER. Backfill `git tag v0.99.98.5 f694a01b`. Update RELEASING.md's site list and tag count.

**Done when.** check_release.sh exits 0 on the release commit and non-zero if one site is edited; `git rev-parse v0.99.98.5` = f694a01b; [Unreleased] lists the event rename before the next version heading.


## G-24 (P1, confirmed): tools/build_deployment.sh skips the Arduino patch guard and accepts a dirty tree

**Claim at 95dc3f34.** tools/build_deployment.sh (unchanged since 5a1065e8) still calls idf.py with no verify_patches.sh run and no git-cleanliness check. build_board.sh, and build_config.py through it, does run the patch guard. The vendored Arduino core is git-ignored, so its 16+ BLEClient patch markers exist only on the build machine.

**Why this priority.** The signed-release path is the one place a de-patched Arduino core or a dirty tree must be refused, components/arduino is git-ignored so its patches are local state, and the fix is a few lines.

**Evidence.**
- `tools/build_deployment.sh:119` idf.py calls at 123/132/141; no verify_patches, git status or dirty refusal anywhere in the 217-line script
- `tools/build_board.sh:93` Only build-path caller of verify_patches.sh; tools/build_config.py:340 builds through build_board.sh
- `.gitignore:99` components/arduino/ ignored; git ls-files components/arduino = 0 files
- `docs/arduino-local-patches/verify_patches.sh:1` BLEClient.cpp markers raised 14 -> 16 plus new symbol checks (d5f7b9ac)
- `tools/gen_build_info.py:266` Dirty count recorded, never enforced

**Newer commits.** d5f7b9ac: new Arduino BLE client patch (connect semaphores) increases what a de-patched release build would silently lose

**Waveshare.** Waveshare has no deployment contract, so its builds go through build_board.sh, which does run the patch guard. The gap applies to the headless, pocket_assistant and handheld contracts.

**Bounded fix.** Before the first idf.py in build_deployment.sh, run `bash docs/arduino-local-patches/verify_patches.sh || exit 1`. Refuse a non-empty `git status --porcelain` unless HW1_ALLOW_DIRTY_RELEASE=1. Write the full SHA and dirty flag into the release directory.

**Done when.** With one patch marker removed, build_deployment.sh exits non-zero before '==> Building'; with one modified tracked file it exits non-zero; with the override set it proceeds and records dirty=1.


## G-25 (P1, confirmed): No executable proof that the tracked patch set reproduces the vendored Arduino tree

**Claim at 95dc3f34.** Still true, and now demonstrably so. verify_patches.sh only checks marker floors and symbols in the bench tree. The latest tracked reproduction record is still 2026-10-15, with three more patches since. A read-only replay on scratch copies shows the documented order leaves BLEClient.cpp with 15 markers and no manifest match: it skips the 2026-11-06 patch that arduino.patch was regenerated on top of.

**Why this priority.** Every hardware qualification rests on a vendored tree that no tracked check proves reproducible, and the documented sequence is already shown not to reproduce it (G-02), so a scripted fidelity check is needed before 1.0.

**Refuted or overstated.** The variants/XIAO_ESP32S3_SENSE worry does not apply: no tracked board uses it and the 95dc3f34 checkout's vendored copy has no such directory. ORCH-1 did a clean-clone replay at aa6508fd on 2026-10-30 (apply --check clean, verify passed, feathers3 built), but it is not recorded in the tree and was not a diff -r.

**Evidence.**
- `docs/arduino-local-patches/verify_patches.sh:15` Lines 15-30 check_markers is a >= floor on the live tree; 57-64 check_symbol and 86-111 check_absent likewise; nothing replays the tracked patches onto a clean base
- `docs/arduino-local-patches/README.md:72` Lines 72-75: 'Clean clone plus all three patches reproduces the main checkout's tree on 2026-10-15' is still the only record; the 10-22, 10-25 and 11-06 patches came later
- `docs/arduino-local-patches/README.md:36` Says arduino-local-patches.patch is the full snapshot 'of all of the above'; the file dates from 2026-10-12 and holds none of the later rows
- `docs/arduino-local-patches/arduino-local-patches-2026-11-06-ble-connect-semaphores.patch:1` Plain ---/+++ hunks; missing from the README sequence (54-62). Scratch replay: reverse arduino.patch and this patch from the vendored files, re-apply arduino.patch alone, and BLEClient.cpp has 15 markers with no manifest 'after' match
- `tools/ble-phone-pipe/manifest.json:20` Exact-file hashes for six BLE files are the only byte-level check, and they cover only those files

**Newer commits.** d5f7b9ac: added a 6th dated patch and regenerated arduino.patch on top of it, without updating the README order or adding a fidelity check; f688bcc1: works around Arduino NetworkEvents with a project-wide NETWORK_EVENTS_MUTEX instead of a core patch; the unmarked NetworkEvents.cpp change still cannot be checked when components/arduino is a plain copy (verify_patches.sh:113-120)

**Bounded fix.** Add tools/check_arduino_fidelity.sh, which can share code with the G-01 vendor script. It clones 11bc7ac1a into a temp dir, applies the README sequence (with the 11-06 patch), and runs diff -r against components/arduino excluding .git. Run it in the release checklist and record the date in CHANGELOG/RELEASING.md.

**Done when.** The fidelity script exits 0 (empty diff) against the release commit's components/arduino, and the release CHANGELOG entry cites the run date.


## G-26 (P1, partly_confirmed): P4X-EYE SDK preparation is unscripted and the 'clean' build uses a locally patched ESP-IDF

**Claim at 95dc3f34.** Every P4 build (P4X-EYE and Waveshare) still needs the four staged overrides. prepare_esp_dl.py still finds pristine esp-dl only in managed_components or the component cache, and otherwise demands --source. Root CMake FATAL_ERRORs before project() if the overrides are missing. The IDF PIE patch is checked only when ENABLE_STT_SPEAKER_LABELS is on (the p4x_eye board profile), and only if portasm.S exists. prepare_sdk.sh --check does not check it, and BUILD_INFO does not record IDF state.

**Why this priority.** On a fresh machine a P4 configure stops before the component manager can fetch the pristine packages, and prepare_sdk.sh cannot obtain them on its own. A public cloner therefore cannot build either P4 board from the docs alone.

**Refuted or overstated.** ORCH-6's worry that S3/classic-ESP32 images may differ is refuted from code. The PIE patch touches only the RISC-V FreeRTOS port, which Xtensa targets never compile. The prepare_sdk overrides are staged copies added to EXTRA_COMPONENT_DIRS only when the target is esp32p4 (CMakeLists.txt:747) and are never edits to the shared SDK. The IDF patch is needed only for speaker-label builds: not the handheld contract (speaker labels default 0) and not Waveshare.

**Evidence.**
- `tools/p4/prepare_esp_dl.py:111` candidates(): managed_components or cache glob; error at 120-127 asks for --source
- `CMakeLists.txt:747` esp32p4: FATAL_ERROR if any of bt/esp_driver_jpeg/esp-dl/esp_hosted overrides missing (747-759)
- `components/hardwareone/CMakeLists.txt:629` PIE q3 check only under HW_CFG_ENABLE_STT_SPEAKER_LABELS and `if(EXISTS portasm.S)` (629-640)
- `tools/p4/prepare_sdk.sh:12` --check runs the four prepare_* checks; no IDF patch check
- `tools/gen_build_info.py:264` No IDF commit or dirty state recorded
- `tools/p4/idf-patches/0001-freertos-fix-xesppie-registers-save-restore.patch:14` Touches only riscv portasm.S and rvruntime-frames.h PIE save area

**Waveshare.** Waveshare builds need the same four staged overrides, including the ESP-Hosted DMA stride fix for its SDIO C6, so they share the bootstrap gap. They do not enable speaker labels, so they do not need the IDF PIE patch.

**Bounded fix.** Let prepare_esp_dl.py and prepare_esp_hosted.py fetch the pinned packages with the component manager CLI into a temp dir and verify SOURCE_TREE_SHA256. Add an IDF PIE-patch state line to `prepare_sdk.sh --check`. Record `git -C $IDF_PATH rev-parse HEAD` and its dirty state in BUILD_INFO. Add one QUICKSTART line saying prepare_sdk.sh is needed for P4 boards only.

**Done when.** On an empty component cache with stock IDF 5.5.5, the documented P4 sequence builds `tools/build_board.sh p4x_eye` without a manual download; `prepare_sdk.sh --check` reports the PIE patch state; BUILD_INFO shows the IDF commit and dirty flag.


## G-28 (P1, confirmed): No SECURITY.md, private reporting channel, issue templates, known-issues, troubleshooting or diagnostics pages

**Claim at 95dc3f34.** The tracked tree has no SECURITY.md, no .github/ (so no issue templates or private-reporting pointer), no KNOWN_ISSUES, TROUBLESHOOTING or DIAGNOSTICS page, and no user-facing statement of the trusted-network posture. The only 'Known issues' text is docs/P4X_EYE_PERIPHERALS.md:276, and the only troubleshooting section is the build-time list at docs/BOARD_SWITCHING.md:429. The diagnostic commands exist, and the new crash-record and system-log-rotation work adds more, but nothing tells a user what to attach to a report.

**Why this priority.** A public 1.0 of a device with HTTP-by-default, plaintext BLE until provisioned and flash encryption off needs a security policy and an honest posture statement; issue templates and troubleshooting pages help but are P2 on their own.

**Evidence.**
- `(repo root):None` `git ls-files | grep -iE '^(SECURITY|NOTICE|CODE_OF|\.github)'` is empty; no KNOWN_ISSUES/TROUBLESHOOTING/DIAGNOSTICS docs outside vendored and experiment dirs.
- `README.md:None` `git grep -iE 'vulnerab|security@|responsible disclosure|bug report|open an issue' -- README.md CONTRIBUTING.md RELEASING.md docs/*.md` finds nothing relevant.
- `docs/P4X_EYE_PERIPHERALS.md:276` The only 'Known issues' sentence (P4 only).
- `docs/BOARD_SWITCHING.md:429` '## Troubleshooting': build-time only.
- `RELEASING.md:57` Release-notes hygiene line only; no security or publishability step.
- `components/hardwareone/Bluetooth.cpp:3974` The device does print a boot notice that BLE is UNENCRYPTED without a blesecret, so some of the posture is stated at runtime. It is not in any doc.

**Newer commits.** d987fdf5, f688bcc1, b00a95dc, a33742bc: crash-record capture and system-log rotation add more diagnostic primitives, still with no user-facing diagnostics guide; Waveshare docs (WAVESHARE_P4_SUPPORT.md) add no known-issues section

**Waveshare.** A new board family raises the need for a per-board known-issues list. Neither Waveshare doc has a known-limitations heading.

**Bounded fix.** Add SECURITY.md (supported versions, a private reporting channel, the trusted-network posture, and the known limitations: HTTP default, BLE plaintext until blesecret, no flash or NVS encryption except feathers3_fe, physical access = credential access). Add a short docs/KNOWN_ISSUES.md and a .github/ISSUE_TEMPLATE/bug_report.yml asking for board, version, crashlog/bootcount/events output and a serial capture.

**Done when.** SECURITY.md and .github/ISSUE_TEMPLATE/bug_report.yml are tracked; GitHub shows the security policy; KNOWN_ISSUES.md has a 1.0.0 heading; every command named in the template appears in docs/COMMAND_REFERENCE.md.


## G-29 (P1, confirmed): OTA/update documentation is false in six places and the user guide has no update/recovery section

**Claim at 95dc3f34.** At 95dc3f34 the OTA_DEVICE_SETUP size table and the OTA CSV header still show pre-2026-08-16 sizes, README:139 still denies XIAO OTA, the otastatus unavailable text still omits qtpy_esp32 and the XIAO deployment, the arduino-local-patches README still calls its tracked directory untracked, the P4 MIGRATION still cites a non-existent deploy tool, and USERGUIDE has no update/recovery section.

**Why this priority.** Wrong OTA sizes and availability statements on the update path mislead operators of the one subsystem that is hard to fix in the field, and the user guide has no update/recovery procedure at all.

**Refuted or overstated.** The OTA_DEVICE_SETUP table is explicitly captioned as the 'original' layout, so it is stale-by-omission rather than mislabelled; the CSV header comment is the unambiguous error.

**Evidence.**
- `docs/OTA_DEVICE_SETUP.md:31` Table: ota_0 5,760 KiB, littlefs 9,216 KiB, 'balances exactly' (38); the CSV rows give ota_0 0x620000 = 6,272 KiB and littlefs 0x880000 = 8,704 KiB. The table is captioned 'The original 16 MB FeatherS3 layout' (29) but no current figures are given.
- `partitions/partitions_ota_no_sr_16mb.csv:7` Header comments '(5760 KiB)'/'(9216 KiB)' contradict rows 22-23 and the 2026-08-16 note at line 10 in the same file.
- `README.md:139` 'The XIAO ESP32-S3 has no OTA layout yet' vs deployments/pocket_assistant/boards/xiao_s3/contract.conf:7-8.
- `components/hardwareone/System_OTA.cpp:1824` Unavailable text (also 1828) names 'feathers3, feathers3_fe, feather_esp32_v2'; CMakeLists.txt registry also has qtpy_esp32, and the pocket_assistant XIAO deployment is an OTA layout.
- `docs/arduino-local-patches/README.md:90` 'This docs/ directory is itself untracked'; git ls-files lists the README, verify_patches.sh and seven .patch files (one added 2026-11-06).
- `deployments/handheld/boards/p4x_eye/MIGRATION.md:47` '(or the deploy tool's CSV check)'; tools/p4 has no flash/deploy tool and git ls-files tools has no partition-compare script.
- `docs/USERGUIDE.md:1479` Only OTA text is the battery policy (1479-1483); no update or recovery section; grep otastage/otaupdate/HW1-Recovery in USERGUIDE returns nothing.
- `deployments/pocket_assistant/boards/xiao_s3/MIGRATION.md:66` Tells the user to check 'ota status'; the main-app command is otastatus (System_OTA.cpp:2170).

**Newer commits.** none fix it: since aa6508fd only docs/arduino-local-patches/README.md changed (one line for the 2026-11-06 patch; the 'untracked' paragraph remains)

**Waveshare.** Waveshare is factory-only; the per-board update table proposed should include it as cable-only (application-only flash after a layout check), as WAVESHARE_P4_SUPPORT.md:36 already requires.

**Bounded fix.** Regenerate the OTA_DEVICE_SETUP table from the CSV (factory 1,336 / otadata 8 / ota_0 6,272 / littlefs 8,704 KiB) and fix the CSV header; reword README:139; list all registry rows and the deployment in System_OTA.cpp unavailable(); delete the 'untracked' paragraph; drop or implement the P4 'deploy tool' reference; fix 'ota status' to otastatus; add a short USERGUIDE 'Updating and recovering' section with a per-board update-path table.

**Done when.** A CSV-vs-table check reports equal sizes; grep 'no OTA layout yet' README.md, 'untracked' docs/arduino-local-patches/README.md, 'deploy tool' deployments/handheld/boards/p4x_eye/MIGRATION.md all return nothing; System_OTA.cpp unavailable text lists qtpy_esp32; USERGUIDE has the update/recovery section.


## G-32 (P1, confirmed): Discover->confirm pairing control frames are single-shot and the state machine has no test or dated hardware record

**Claim at 95dc3f34.** At 95dc3f34 PAIR_REQUEST and PAIR_ACCEPT are still sent once each, as unacknowledged FF broadcasts with no resend. If the ACCEPT is lost, the target writes its registry and starts KEY_EX while the initiator times out after 45 s; the initiator never adds the peer, and a re-request is answered with PAIR_REJECT. No host suite runs v4h_pair_request/accept/reject or cmd_espnow_pairrequest/accept, and no dated hardware record names this flow.

**Why this priority.** Pairing is the first thing a new user does with two devices: if one frame is lost the two devices end up disagreeing about whether they are paired, and no test or dated record covers the request/accept flow, so a 1.0 that wants to be trustworthy needs a retransmit and a harness here even though a lost frame at pairing distance is uncommon.

**Refuted or overstated.** Saying 'only a CLI command no surface suggests' goes too far. espnowdiscovered and espnowpairmode help both print 'espnowpairsecure <mac> <name>', and the web ESP-NOW page has a manual secure-pair form. What is missing is a recovery hint at the 45 s timeout and on the OLED/G2 flow. A lost PAIR_REQUEST is harmless, because the initiator simply times out and can retry; only a lost ACCEPT leaves the asymmetric state.

**Evidence.**
- `components/hardwareone/System_ESPNow.cpp:5887` sendPairCtrl: one v4_send_frame to FF, BROADCAST_AUTH, ttl 1, no ACK_REQ, no retry (unchanged since aa6508fd; the 30-line diff in this file is only the cmd_exec_task -> cmd_exec comment rename)
- `components/hardwareone/System_ESPNow.cpp:5936` v4h_pair_request answers PAIR_REJECT when pairModeIsPaired(mac), so a re-sent request from the half-paired initiator is refused
- `components/hardwareone/System_ESPNow.cpp:5955` v4h_pair_accept is the only initiator path that runs PairCompleteJob -> cmd_espnow_pairsecure; it needs the ACCEPT frame
- `components/hardwareone/System_ESPNow.cpp:10277` the heartbeat sweep only expires gPendingPairOut after 45 s and prints 'request ... timed out (no response)'. It resends nothing and does not name a recovery command
- `components/hardwareone/System_ESPNow.cpp:18054` cmd_espnow_accept sends ACCEPT once, then pairs locally with cmd_espnow_pairsecure
- `components/hardwareone/System_ESPNow_Handlers_Crypto.cpp:228` v4hKeyExHello persists the peer identity (peerIdentityPersist at 297) but never writes devices[]; auto-pair from beacons is removed (System_ESPNow.cpp 5831), so nothing else adds the peer
- `components/hardwareone/OLED_ESPNow.cpp:1044` the OLED pair action uses espnowpairrequest, so the display flow is exposed to the lost-ACCEPT case
- `components/hardwareone/System_ESPNow.cpp:17972` refutation: espnowdiscovered prints 'pair with espnowpairsecure <mac> <name>', so the CLI does show the repair command, just not at the timeout
- ... 3 more in verify/E1.json

**Newer commits.** 64713c0d: touches System_ESPNow.cpp/Handlers_Crypto.cpp only for comment renames (cmd_exec_task -> cmd_exec); the pairing logic is unchanged; none of fbc6fecc, d5f7b9ac, a9d47ea3, 37b2fa23, d0f050f9, 582f2c98 touches ESP-NOW

**Waveshare.** Does not apply to the Waveshare family as shipped: configurations/waveshare-p4-bringup.json sets NETWORK_FEATURE_LEVEL 2, so ENABLE_ESPNOW is 0 (System_BuildConfig.h 734-736).

**Bounded fix.** Resend PAIR_REQUEST/PAIR_ACCEPT from the heartbeat sweep about every 1.5 s while the pending record is active (bounded, inside the 45 s TTL); on the initiator, let a KEY_EX_HELLO from the gPendingPairOut MAC complete the pairing; make the timeout text print 'espnowpairsecure <mac> <name>'. Add a two-device modelled harness in the espnow_session_rekey style.

**Done when.** A new ctest (for example espnow_pairing_flow) drops the first ACCEPT and the first REQUEST and asserts that both modelled devices hold the peer in devices[] with an ACTIVE session; it fails with the retransmit removed. A dated CHANGELOG line records a two-board request/accept pairing (CLI on both, then the OLED prompt on one) naming the boards and the firmware.

**Hardware needed.** Two native-radio boards (for example XIAO-S3 and QT Py): pair via espnowpairrequest/espnowaccept, then repeat with espnowrelayblock armed on the initiator for the target's MAC during the accept, to show recovery or the asymmetric state.


## G-34 (P1, partly_confirmed): BLE peers leaving range and returning have no dated hardware record; R1 wedge fix untested; phone reconnect scan never backs off

**Claim at 95dc3f34.** Genuine out-of-range-and-back has still not been tested for any peer ('user cannot perform the walking test'). The v0.99.99 ring getServices fix is still marked untested in CHANGELOG, and the same 'attempt already in flight' wedge recurred on 2026-11-05 through a different path (the OPEN semaphore). That path is now fixed and bounded by a 240 s report and a 600 s reboot, but the bound has not run on hardware. The phone still scans every 30 s forever.

**Why this priority.** Walking away from the glasses, ring or phone and coming back is the everyday case. It is still the untested one, and the 2026-11-05 four-hour stall shows these paths do fail in the field.

**Refuted or overstated.** CONN-5 is overstated as 'no hardware evidence': stationary ring recovery and interrupted-discovery and close cases were exercised on 2026-11-05/06 (docs/investigations). The 360 s 'warns only' point is no longer the whole story, because a stuck setup run now reboots at 600 s. Controlled C6 restarts recovered all four links on four cycles. Still true: no walk-away record, the CHANGELOG still says untested, and the phone scan has no backoff decision.

**Evidence.**
- `CHANGELOG.md:36` the 0.99.99 ring wedge fix still says 'Not yet tested on hardware'; [Unreleased] is empty, and none of the six BLE commits added a CHANGELOG line
- `docs/investigations/2026-11-04-p4x-acceptance.md:63` four-link return to range: only controlled C6 restarts were done; 'Genuine range loss/return remains untested'
- `docs/investigations/2026-11-04-p4x-fix-progress.md:565` 2026-11-05: six natural (stationary) ring drops recovered in 8.9-34.9 s with wearStatus ACK; explicitly 'not genuine simultaneous range return'
- `docs/investigations/2026-11-04-p4x-fix-progress.md:984` root cause of the 2026-11-05 four-hour stall: an OPEN timeout left m_semaphoreOpenEvt taken and the client was reused, blocking ble_setup forever (so the 0.99.99 search-lock fix alone was not enough)
- `docs/investigations/2026-11-04-p4x-fix-progress.md:1059` the escalation path has not run on hardware: the stall cannot be reproduced on demand
- `docs/investigations/2026-11-04-p4x-fix-progress.md:1101` 2026-11-06 overnight soak on the recovery image: no stall and no overrun line; ring drops track movement. Stationary only
- `components/hardwareone/BLE_CentralStack.cpp:341` bleSetupOverrunTick: 240 s report sets bleStackSetLifecycleFault(true); 600 s rebootDevice('ble-setup-overrun'). When the run returns only sReported resets; the fault is cleared only by bleStackDown (304) or a recycle (Bluetooth.cpp 2381)
- `components/hardwareone/G2_Ring.cpp:6967` NEW RISK (code reading): 'ring scan' accepts up to 300 s and runs as one Ring job on the setup worker (G2_Glasses.cpp 20015-20017); it ends early only if another client queues. A manual scan over 240 s with nothing else queued would trip the overrun report and leave the lifecycle fault set, after which G2 scans (G2_Glasses.cpp 1033) and phone admission (BLE_PhonePipe.cpp 1048) refuse until closeble/openble or reboot
- ... 4 more in verify/E1.json

**Newer commits.** d5f7b9ac: the library releases the connect open/registration semaphores on timeout (root cause of the 2026-11-05 stall); host test test_ble_client_connect_semaphores; dated patch under docs/arduino-local-patches; d0f050f9: retires the client after OPEN_TIMEOUT and adds the 240/600 s setup-worker overrun watch (ble_setup_worker harness); introduces the manual-ring-scan-over-240 s fault risk; a9d47ea3: ring keeps setup handles valid through disconnect, fails discovery fast, records the HCI reason (ring_discovery_failure, ring_owner_write, ring_close_setup harnesses); 37b2fa23: phone DISCONNECT/CLOSE isolated by peer address, first cause kept (phone_gatt/phone_runtime harnesses); fbc6fecc: one-arm AUTO rediscovery keeps the saved mate (g2_saved_pair_identity); 582f2c98: dated hardware records (2026-11-04 to 11-06) for all of the above; walking test explicitly not done

**Waveshare.** Applies: the Waveshare saved profile enables G2 and the phone pipe over the same C6-hosted BLE stack (ENABLE_R1_HEALTH 0 in that profile), and no range-loss record exists for it either.

**Bounded fix.** Run the walk-away matrix and record it. Separately, either clear the overrun-raised lifecycle fault when the reported run returns, or raise reportMs above the 300 s manual ring scan (or cap 'ring scan' below 240 s), with a ble_setup_worker case for it. Decide and document the phone 30 s scan (accept it for 1.0 or implement A-3).

**Done when.** A dated table (board, peer, away 2 min / 15 min / all three, reconnect seconds) where no row needs a command or reboot; the 0.99.99 'Not yet tested on hardware' line is updated; a host case shows that a 300 s Ring run does not leave bleStackLifecycleFaulted() true after it returns.

**Hardware needed.** P4X-EYE plus one S3 board: carry each of the glasses, ring and phone out of range for 2 min and 15 min and back, then all three together, capturing blerecords/blestatus. Also run 'ring scan 300' with the glasses connected and the phone pipe off, and check blestatus for 'reboot required' after the scan ends.


## G-38 (P1, confirmed): Corrupt settings.json or owner credential file is never quarantined: settings-read-only device, blocked factoryreset, silent owner lockout

**Claim at 95dc3f34.** An unparseable settings.json leaves the device on defaults with every save refused (deliberately, to preserve the file). factoryreset_confirmed then stops at writeSettingsJsonForFactoryReset before users.json is touched. The boot integrity pass excludes settings.json and checks debug.json and automations.json only by first character. An unparseable or non-object owner credential file fails every login closed and is never quarantined, logs no event and does not re-arm setup.

**Why this priority.** This needs flash-level damage or a bad restore to trigger. When it does, the device cannot be fixed in-band (settings are frozen, factoryreset is blocked, the owner is locked out with no event), which undermines a trustworthy 1.0. A bounded quarantine fixes it.

**Refuted or overstated.** The settings save refusal is a deliberate data-preservation design, not an oversight, and it is logged every time (SYSEVT_SETTINGS_SAVE_FAILED). The defect is the missing way out (no quarantine, factoryreset blocked), not the refusal. I did not confirm the claim that automations.json becomes read-only: a corrupt automations.json is silently read as empty.

**Evidence.**
- `components/hardwareone/HardwareOne.cpp:1736` load failure: 'running on defaults (file left on disk)' (:1739); gSettingsLoadedOk stays false
- `components/hardwareone/System_Settings.cpp:1555` writeSettingsJsonImpl refuses while !gSettingsLoadedOk; the merge-read parse failure also refuses (1583-1612). The refusal is intentional and documented (1550-1553)
- `components/hardwareone/System_Utils.cpp:2470` factoryreset_confirmed returns 'pipe secret clear was not persisted' before users.json is removed (:2519), so a corrupt settings.json blocks the reset
- `components/hardwareone/System_Filesystem.cpp:229` boot check deliberately excludes settings.json (comment 229-234); criticalFiles at :235 is debug/automations/users only, first-character check, quarantine to .corrupt
- `components/hardwareone/System_Filesystem.cpp:1617` /system/settings.json: admin PERM_READ; only FsRole::SUPER (PERM_ALL, :1939) can delete it
- `components/hardwareone/System_Settings.cpp:1710` debug.json save refused while !gDebugLoadedOk; readDebugJson parse error (1757-1761) leaves it false, with no quarantine
- `components/hardwareone/System_Automation.cpp:774` an unparseable automations.json is silently treated as empty (markAutomationsCacheEmptyValid), with no event or quarantine
- `components/hardwareone/System_User.cpp:2789` isValidUser: if (!loadUserSettings(...)) return false
- ... 2 more in verify/D1.json

**Newer commits.** none relevant: the System_Settings.cpp changes since aa6508fd are comment renames and supported() filtering

**Bounded fix.** (1) In factoryreset_confirmed, when settings were never loaded or the merge-read fails, rename settings.json to settings.json.bad and continue instead of refusing. (2) Use a full deserializeJson boot check for settings.json, debug.json and automations.json, quarantining to .bad with SYSEVT_CONFIG_FILE_CORRUPT. (3) Add the owner's user_settings/1.json to that pass: on failure, rename it and write PENDING_CRED_SETUP_FILE.

**Done when.** Host runners on fake_vfs cover these cases. A truncated settings.json is quarantined, the next save succeeds and factoryreset completes. A corrupt 1.json is quarantined, the marker is written and setup reaches runPostRestoreCredentialSetup with other files byte-identical. errors.log or events.log records the event.

**Hardware needed.** One boot with a hand-truncated settings.json and one with a corrupt 1.json on a single board, each confirming the device can be recovered in-band.


## G-39 (P1, confirmed): Capped logs become uncapped when rotation cannot allocate; the LittleFS reserve only redirects; the SD overflow latch drops all logs after a card yank

**Claim at 95dc3f34.** appendLineWithCap appends first and rotates only by copying about 85% of the cap to a .tmp. When that copy cannot fit, it returns false with the file already past its cap, and every later line repeats the failing copy under the FS lock. The 100 KB reserve only redirects to SD and never refuses when no card is present. The caps (~2.8 MB) exceed the 2.23 MB and 1.81 MB 8 MB-layout partitions. On SPI-SD and detect-less SDMMC boards, the latch keeps routing logs to a removed card until reboot.

**Why this priority.** On 8 MB layouts the log caps add up to more than the LittleFS partition. A log past its cap then retries a large failing copy on every line until flash is full, and settings saves start failing. The SD-yank latch (PERS-5) is narrower, about P2.

**Refuted or overstated.** The unauthenticated driver named in the original (/api/cli 'unauthorized') is filtered out by logAuthAttempt's isSecurityAuditEvent and never writes login-failed.log. The same effect is still reachable through repeated web /login attempts, including ones rejected during lockout. The once-per-line cost is a failing copy that ends at the first write failure, not always a full multi-hundred-KB copy.

**Evidence.**
- `components/hardwareone/System_Filesystem.cpp:2275` append first (2301-2320), cap check (:2325), tail copy to .tmp (2367-2397); an incomplete copy returns false (2398-2402) after the line was already added. The header comment (2267-2274) claims the log 'stays pinned at its cap', which is inaccurate: each append grows it
- `components/hardwareone/System_VFS.cpp:808` LOG_OVERFLOW_DEFAULT_RESERVE 100 KB 'preserved for NON-log writes'; resolveOverflowPath (886-929) returns the LittleFS path when the latch is set and !isSDAvailable() ('accept the silent-loss behavior')
- `components/hardwareone/System_Logging.h:54` caps 696,969 x2 + 64 KB + 256 KB x3, plus command-audit 500 KB (System_Utils.cpp:878) and crash-history 64 KB (System_CrashRecord.cpp:525), about 2.8 MB in total
- `partitions/partitions_no_sr_8mb.csv:32` littlefs 0x23B000 (2.23 MB); partitions_ota_no_sr_8mb.csv littlefs 0x1D0000 (1.81 MB); partitions_sr_8mb.csv only 0x20000
- `components/hardwareone/WebServer_Server.cpp:3756` a web /login attempt rejected during lockout still calls recordLoginAttempt -> 'web/login' -> LOG_FAIL_FILE, so an unauthenticated client can still grow the login-fail log without a PBKDF2 cost
- `components/hardwareone/System_Debug.cpp:2512` isSecurityAuditEvent filter: '/api/cli' + 'unauthorized' (WebServer_Server.cpp:3372, :5236) does NOT reach the file; this refutes the original /api/cli path
- `components/hardwareone/System_VFS.cpp:428` isSDAvailable: the SD.available() yank check exists only for ENABLE_SDMMC_CARD; the SPI build returns gSdMounted. resolveOverflowPath never consults isSDWritable()
- `components/hardwareone/HAL_SDCard.cpp:85` SDMMC available(): removal is detected only through a detect pin; Waveshare has SD_MMC_DETECT_PIN -1 (System_Board_WaveshareP4.h:33), so a yank goes undetected there too
- ... 2 more in verify/D1.json

**Newer commits.** b00a95dc: bounds the opt-in `log start` system log (rename to .1 at 8 MB); does not touch appendLineWithCap or the reserve; a33742bc: makes that bound systemLogMaxSize (default 1 MB) x systemLogMaxRotations (default 3); rename-only rotation is the no-extra-space pattern PERS-4 asks for, but the default 4 MB budget exceeds 8 MB-layout LittleFS and ignores the 100 KB reserve, so on a small board an autostarted system log in the LittleFS capture tree can still fill flash; 95dc3f34: records the deployment only

**Waveshare.** Adds to PERS-5. The Waveshare SDMMC slot (82919267) has no card-detect pin, so a yanked card stays 'available' just like the SPI XIAO case (docs/WAVESHARE_P4_SUPPORT.md:269-274 tell users to sdunmount first). Its 23.9 MB LittleFS makes the latch, and PERS-4, much less likely there.

**Bounded fix.** In appendLineWithCap, when over the cap and free space is below what the copy needs, rotate by rename (dest -> dest.1, as SensorLogging and the new system log already do) or drop the line behind a 'rotation blocked' latch. When the latch is set and no SD is present, refuse non-exempt appends. resolveOverflowPath should require isSDWritable(), and a failed /sd append should call noteSDWriteFailure and retry LittleFS. The system-log writer should also respect the reserve.

**Done when.** capped_log_writes gains a low-space case (file never exceeds cap + one line, no repeated copy) and a latched-plus-SD-open-failure case (line lands on LittleFS or a counted drop). A LittleFS pre-filled to <100 KB on a host or XIAO check still saves settings.json.

**Hardware needed.** XIAO S3 (2.23 MB LittleFS) pre-filled to near full: drive login failures and confirm the logs stay bounded and settings still save. XIAO Sense with the latch set and the card pulled: command-audit.log keeps receiving lines.


## G-41 (P1, confirmed): No power-interruption or physical-media qualification for any atomic-save claim

**Claim at 95dc3f34.** No tracked doc, CHANGELOG entry or validation record shows a real board surviving repeated power cuts during settings, users.json or credential saves. Every durability statement is qualified as host or observable-completion only, and SD qualification is still deferred, now with an SD slot enabled by default on the Waveshare profile.

**Why this priority.** If the save path fails on real flash, a power cut means lockout (G-10). One bounded power-cut soak on the primary board is a cheap way to back the CHANGELOG's atomic-save statements for 1.0.

**Refuted or overstated.** No user-facing 1.0 doc (README, USERGUIDE) explicitly claims a power cut cannot lose state; the claim lives in CHANGELOG wording. Searching *.md, *.json and records turned up no power-cut soak.

**Evidence.**
- `CHANGELOG.md:999` 0.99.97.3: host failure injection 'does not establish physical power-loss durability'
- `CHANGELOG.md:2333` 'Every user-database write is now atomic' is the closest thing to a durability claim, and it has no physical test behind it
- `CONTRIBUTING.md:61` 'Memory slimming and SD qualification remain deferred until explicitly resumed'
- `docs/MODULARIZATION_PLAN.md:4516` 'checked observable completion, not a power-loss durability guarantee'; :4405 'controlled reboot checks, not physical power-loss tests'
- `docs/QTPY_STORAGE_TEST.md:97` the QT Py storage record leaves power-loss durability unqualified
- `components/hardwareone/System_Filesystem.cpp:2412` SD capped-log rotation is remove-then-rename; only the boot orphan pass (recoverLogOrphanAt) covers the gap, and that has not been tested on real media
- `docs/WAVESHARE_P4_SUPPORT.md:264` the Waveshare default profile enables SDMMC after one bounded write/read/remount check (2026-11-01); this is not a power-loss or endurance qualification

**Newer commits.** 82919267: Waveshare SDMMC slot enabled in its default profile, widening the SD qualification gap

**Waveshare.** Adds SD scope. The Waveshare profile has SD on by default, a 32 MB flash with a 23.9 MB LittleFS, and no card-detect pin. The board is not listed in README's supported-boards table.

**Bounded fix.** Run a scripted power-cut soak of 200 or more cycles on a FeatherS3 and one 8 MB board (XIAO S3 or QT Py). Each cycle triggers a settings save and a users.json or credential mutation over serial, cuts power at a random offset under 300 ms, reboots and reads the files back. Record the result with a date. Label SD as experimental per board (XIAO Sense, Waveshare) unless it is qualified.

**Done when.** A dated record shows zero unparseable state files and zero setup re-arms over at least 200 cycles per board, or a defect is filed. README and CONTRIBUTING state the SD status per board.

**Hardware needed.** Switchable power supply or relay on a FeatherS3 and an 8 MB board, with 200 or more cut cycles each, reading back over serial.


## G-42 (P1, partly_confirmed): No dated soak or endurance record for any profile and no repeatable capture bundle

**Claim at 95dc3f34.** There is still no structured soak record (a memreport/taskstats series on a tagged build) for any profile, and none at all for S3 or classic boards. The P4X-EYE now has unstructured investigation runs: about 3.8 h on 2026-11-05 and 8 h 44 min overnight on 2026-11-06. Soak tooling is partial: memreport minFree is boot-wide and cannot be reset, and allocationTraffic counts cumulative requests. The CPU sampler and task renames are now committed.

**Why this priority.** A 1.0 stability claim needs at least one structured multi-hour run per supported family; the P4X runs since 2026-11-05 show soaks find real faults (a 3.4 h stuck setup worker, an unrecorded restart), and S3/classic boards still have none.

**Refuted or overstated.** RES-10's 'shared CPU sampler and task renames are uncommitted' is out of date: 64713c0d committed them. 'No repeatable capture bundle' overstates the gap: tools/p4/ram_monitor.py already captures memreport JSONL without a reset, though it has no scoring and covers only memreport. 'All hardware evidence is minutes to hours' is now slightly stale for the P4X, which has 3.8 h and 8.7 h investigation runs, but neither is a structured soak. Citations to docs/P4_MEMORY_AUDIT_2026-10-16.md cannot be re-opened: the file is not in the tree at 95dc3f34 or at aa6508fd.

**Evidence.**
- `docs/investigations/2026-11-04-p4x-fix-progress.md:601` 2026-11-05 unattended capture, ~3.8 h, no panic. A later passive read (652-700) found the board had restarted unrecorded around 19:14 UTC and its setup worker stuck for 3.4 h.
- `docs/investigations/2026-11-04-p4x-fix-progress.md:1101` 2026-11-06 overnight on setup-worker-recovery-candidate: one boot, uptime 8 h 44 min, no crash/panic/watchdog. Internal free 215,675 B at the end vs 222,491 B at boot (two points only; no minFree, no series). Glasses+ring+phone idle plus an hourly automation; no speech/web/ESP-NOW load.
- `docs/investigations/2026-11-04-p4x-acceptance.md:109` The acceptance doc itself says a panic-free soak is useful evidence, not proof of the remaining gates.
- `docs/WAVESHARE_P4_SUPPORT.md:289` 'Long-running SD/radio/display stress remains unqualified' for the new Waveshare board.
- `CHANGELOG.md:99` Still 'bench check; ... field endurance ... ' not covered.
- `components/hardwareone/System_Utils.cpp:3616` memreport json dram.minFree = hw1InternalMinFreeBytes() (boot-wide, no reset); allocationTraffic semantics 'cumulative_requested_since_reset' at 3631.
- `tools/p4/ram_monitor.py:1` Missed by the original: since v0.99.98.5 an existing tool sends `memreport json` through a non-resetting logger and writes private JSONL. It is a partial capture driver (memreport only, P4 logger FIFO, no taskstats/perftop, no scoring).
- `components/hardwareone/System_TaskUtils.h:476` TASK_CPU_SAMPLE_MS / taskCpuShares now committed (64713c0d); task names cmd_exec/log_out/g2_owner/c6_console are in HEAD.

**Newer commits.** 64713c0d: committed the task-CPU sampler, task renames and the P4_RUNTIME_MEMORY/TASK_INVENTORY docs; 582f2c98: records the 2026-11-05 3.8 h capture, the unrecorded restart and the 3.4 h stuck setup worker found by soaking; d0f050f9: bounded the stuck setup job the soak exposed; 95dc3f34: records the 8 h 44 min overnight run on the recovery image; a33742bc/b00a95dc: bounded the system log, which grew 3.5 MB in 10 h; that blocked multi-day runs with debug capture on

**Waveshare.** Adds a fifth P4-class profile with no long-run record ('Long-running SD/radio/display stress remains unqualified', WAVESHARE_P4_SUPPORT.md:289). The soak matrix needs a Waveshare row or an explicit exclusion.

**Bounded fix.** Generalise tools/p4/ram_monitor.py (or add tools/soak/capture.py) to issue memreport/taskstats/perftop/events/uptime JSON on a schedule over pyserial without resetting, plus a small checker for the RES-3 criteria. Run it once on a tagged RC for feathers3, feather_esp32_v2 and p4x_eye; Waveshare and xiao_s3 are optional or explicitly excluded. Optionally add a resettable software low-water to memreport.

**Done when.** Tracked docs/SOAK_<date>_<profile>.md per profile with attached JSONL, for at least 8 h (P4) and 24 h (S3/classic): bootcount unchanged and crashlog empty, dram.free slope after warm-up <= 100 B/h, minFree >= 32 KB (S3/P4) / 20 KB (classic), allocationTraffic.failures == 0, every task we create >= 768 B stack min-free.

**Hardware needed.** One device per profile (FeatherS3, Feather ESP32 V2, P4X-EYE; optionally Waveshare and XIAO S3) on USB serial for 8-24 h with the scripted workload.


## G-43 (P1, confirmed): BLE open/close per-cycle leak verified fixed only on the P4; S3/classic cycle drift unmeasured

**Claim at 95dc3f34.** openble/closeble and openg2/closeg2 cycle drift has been measured only on the P4X-EYE, where the BLE controller lives on the C6 companion. No post-rework 20-cycle record exists for FeatherS3 or Feather ESP32 V2, whose controller runs on-chip. The 'bluetooth' gate value and its comment still encode the old 11,192 B/cycle arithmetic.

**Why this priority.** The only native-controller measurement shows an 11 KB/cycle leak, and the P4 'no drift' results exercise the ESP-Hosted controller on the C6, not the native one; a 30-minute bench run settles it for boards the README lists.

**Refuted or overstated.** Nothing substantive. MEMORY_PRESSURE_AUDIT_2026_10_21.md, cited as evidence, is not in the tree at 95dc3f34.

**Evidence.**
- `components/hardwareone/System_MemoryMonitor.cpp:116` 'openble/closeble leaks 11,192 B per cycle ... By cycle 5 ... largest=8,192 -- below BTC_TASK's 8,704 B' (116-121) still justifies the 53248 'bluetooth' row (125)
- `components/hardwareone/Bluetooth.cpp:116` 'ESP32 Bluedroid leaks ~10KB DRAM per init/deinit cycle' toggle tracking still present; the leak print is at 2412-2416
- `components/hardwareone/HAL_Bluetooth.cpp:1` bluetoothHalDeinit calls BLEDevice::deinitChecked(false) and never releases controller memory, on every target. On P4 the controller is Hosted (C6), so P4 cycle results do not cover the native-controller deinit path.
- `components/hardwareone/BLE_CentralStack.cpp:270` bleStackDown is the shared teardown. The blestack gate (203) checks total free only, not the largest block, which the old failure mode depended on.
- `CHANGELOG.md:147` 'stays there across five open/close cycles' is a P4X-EYE result only (also 270, 343)

**Newer commits.** d5f7b9ac: Arduino BLE client releases connect semaphores on timeout; it changes the client lifecycle but adds no S3/classic cycle measurement; d0f050f9: setup worker retires a client after OPEN timeout; same caveat

**Waveshare.** Waveshare also uses a C6 Hosted controller, so it adds no evidence for the native-controller boards and needs no separate row here.

**Bounded fix.** On feathers3 and feather_esp32_v2 (Wi-Fi+HTTP up), run 20 openble/closeble and 20 openg2/closeg2 cycles, capturing memreport json 5 s after each close and the taskstats task count. Then rewrite the System_MemoryMonitor.cpp:113-121 justification from the measured floor.

**Done when.** Tracked record: per board, |dram.free(after 20) - dram.free(after 1)| <= 1 KB, internalLargest(after 20) >= internalLargest(after 1) - 8 KB, constant task count after each close; gate comment updated with the dated figures.

**Hardware needed.** FeatherS3 and Feather ESP32 V2 on USB serial with G2 glasses in range, about 30-60 min each.


## G-44 (P1, confirmed): Classic-board G2 admission floor admits a Wi-Fi+HTTP+G2 combination that lands near 9 KB free

**Claim at 95dc3f34.** On classic ESP32 boards with Wi-Fi and HTTP up (~95.8 KB internal free), the split gates admit G2 start, which lands near 9 KB free. blestack 53,248 is checked before host init, g2client 40,960 after it, total free only. The source comment says this combination 'has been observed to fail afterwards at openg2'.

**Why this priority.** The code knowingly admits a combination it documents as failing afterwards on boards the README offers G2 on; a clear refusal is a one-constant change, so leaving it would ship a known limp state.

**Refuted or overstated.** The 'headless' scope tag overstates exposure: the README Headless Node column marks G2 as not included. The DoD wording 'no BLE stack left up' conflicts with the current design, where a g2client refusal intentionally keeps the shared stack up for the ring and phone.

**Evidence.**
- `components/hardwareone/System_MemoryMonitor.cpp:96` 'deliberately a tight floor: it still admits the G2 client start on a WiFi+HTTP device (~95.8 KB free), which lands near 9 KB and has been observed to fail afterwards' (96-99)
- `components/hardwareone/System_MemoryMonitor.cpp:108` blestack 53248 / g2client 40960 rows. The measured chain 148,115 -> 104,843 -> 67,135 (lines 83-86) predates the ble_setup worker (+5,120 B), so the real landing point may be lower than 9 KB.
- `components/hardwareone/BLE_CentralStack.cpp:203` blestack gate before bluetoothHalInit: free-only check
- `components/hardwareone/G2_Glasses.cpp:19040` g2client gate after the stack is up: free-only, no largest-block check; on refusal the stack stays up by design (ring/phone share it)
- `config/sdkconfig.defaults:214` CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384: allocations <= 16 KB come from internal RAM, so ~9 KB free is effectively exhausted
- `README.md:109` 'BLE server + Even Realities G2 glasses client' is selectable (gear icon) on Sensor/Standard/Bonded builds; the board table (129-134) lists Feather ESP32 V2 / QT Py with no G2 caveat

**Waveshare.** Not relevant: P4 boards have ~450 KB internal RAM and a Hosted controller.

**Bounded fix.** Under CONFIG_IDF_TARGET_ESP32 raise g2client so that blestack + g2client is about 102,400 B (or add a largest-block term). The refusal already prints 'Client start refused: Insufficient heap' plus the closehttp/closewifi hint. Pin the row values in the existing host test. Alternatively, add one README line saying G2 together with Wi-Fi+web is unsupported on the 2 MB classic boards.

**Done when.** On feather_esp32_v2 with Wi-Fi+HTTP up, openg2 either completes with memreport dram.free >= 20 KB and internalLargest >= 16 KB afterwards, or is refused with the 'Insufficient heap' reason; a host test asserts the new row value for the ESP32 target.

**Hardware needed.** Feather ESP32 V2 (or QT Py ESP32) with Wi-Fi+web up and G2 glasses: one openg2 attempt with memreport json before and after.


## G-45 (P1, confirmed): Resident task stacks run with under 1.5 KB free with canary-only detection; c6_console raise uncommitted

**Claim at 95dc3f34.** Resident stack margins on the P4X are tighter than the original claim. On 2026-11-04/05 the boards measured g2_owner 312-688 B of 6,144, log_out 524-540 B of 4,096, cmd_exec 700 B of 8,192 and ble_setup 1,124-1,160 B of 5,120. log_out now also runs the new multi-rename system-log rotation, which hardware has not yet exercised. Overflow detection is the FreeRTOS canary only on S3/classic; P4 builds also have the hardware stack guard.

**Why this priority.** The same class of bug already crashed both P4X boards in the field (qnote 5,120 B overflow), and newer measurements show g2_owner down to 312 B and log_out to 524 B free; a few KB of stack is cheap insurance for 1.0.

**Refuted or overstated.** The c6_console raise is no longer uncommitted (64713c0d, HAL_Radio.cpp:320). 'Canary-only detection, panic may land in an unrelated task' is wrong for P4/Waveshare: the RISC-V hardware stack guard is on, and d987fdf5 records the faulting task. It holds for S3/classic. The quoted figures (1484/1100/1408) are out of date in the other direction: later measurements are lower.

**Evidence.**
- `docs/investigations/2026-11-04-second-p4x-bluetooth.md:175` Second P4X, 0.99.99: g2_owner 312, log_out 524, cmd_exec 700, ble_setup 1,124 B min free
- `docs/investigations/2026-11-04-p4x-fix-progress.md:597` First P4X 2026-11-05: log_out 540 B, g2_owner 688 B min free
- `docs/investigations/2026-11-04-p4x-fix-progress.md:742` The recurring P4X crash was the qnote worker overflowing 5,120 B (stack protection fault); a 60 s note peaks at 5,628 B (958)
- `components/hardwareone/System_TaskUtils.h:70` DEBUG_OUT_STACK_WORDS 4096: 'Do NOT trim below this 4 KB unless those file writes are moved off this task'
- `components/hardwareone/System_Debug.cpp:151` New systemLogRotateLocked (a33742bc) does up to 9 guarded exists/rename/remove calls with String paths on log_out (called at 411). Deployed with 'rotated 0 time(s)', so not exercised on hardware.
- `components/hardwareone/G2_Glasses.cpp:13292` G2_CONTROL_STACK_BYTES 6144 unchanged
- `components/hardwareone/HAL_Radio.cpp:320` c6_console 4096 is now committed (comment at 319)
- `config/sdkconfig:1849` S3: CHECK_STACKOVERFLOW_CANARY=y, WATCHPOINT_END_OF_STACK unset (1879); no HW stack guard on Xtensa
- ... 2 more in verify/D2.json

**Newer commits.** f688bcc1: qnote stack 5,120 -> 8,192 B after confirmed overflows; logs its min free at each note end; d987fdf5: crash record captures faulting task and stack-guard bounds (P4); 64713c0d: committed c6_console 4096; a33742bc/b00a95dc: added rename-based rotation on the log_out task (new stack depth on the tightest task)

**Waveshare.** Waveshare uses the same task constants and also has the HW stack guard. It is unmeasured, but its G2/phone links run the same g2_owner/ble_setup paths.

**Bounded fix.** Raise g2_owner to 7,168-8,192 B and log_out to 5,120 B, and give ble_setup about +1 KB (all internal RAM, about 5 KB on P4; check S3/classic budgets). Or move log_out's LittleFS/SD appends and rotation behind a deferred path. Have taskstats flag any of our tasks below 768 B min-free without the debug flag.

**Done when.** Constants raised with a dated measurement comment; after an 8 h P4X soak and a 1 h FeatherS3 G2 session, taskstats json shows every task we create >= 768 B min-free, including after at least one forced system-log rotation (log maxsize 10240).

**Hardware needed.** P4X-EYE with glasses+ring+phone and a forced log rotation; FeatherS3 with G2 (to measure the Xtensa depth of g2_owner and log_out).


## G-47 (P1, partly_confirmed): Portable/battery claim is unmeasured: no current or runtime figure, battery backends unvalidated, power caveats untracked

**Claim at 95dc3f34.** No current-draw, discharge or runtime measurement exists for any board. No battery backend has been checked against a meter: FeatherS3 gauge+VBUS, Feather V2 ADC, P4X ADC, and the new Waveshare ADC, which was tested without a cell. At critical charge the firmware only notifies. The power-policy docs are now tracked, but README still makes the portable claim with no 'runtime not measured' caveat.

**Why this priority.** README publicly offers a 'battery-powered handheld' with no runtime, current or validated battery reading on any board; for a trustworthy 1.0 the cheap fix is to scope the claim in tracked docs, and measurements can follow.

**Refuted or overstated.** PWR-9's 'the three power-policy documents are untracked' is fixed: 64713c0d tracks CROSS_BOARD_POWER_POLICY.md, P4_BATTERY_BUDGET.md, POWER_MANAGEMENT_IMPLEMENTATION_PLAN.md and G2_CASE_PRESENCE_POWER_PLAN.md. Their caveats are still not linked from README.

**Evidence.**
- `README.md:46` 'Can be used from USB power or as a battery-powered handheld.' unchanged; battery rows 116, 129-134 carry no measurement caveat
- `docs/P4_BATTERY_BUDGET.md:13` 'There is no measured whole-board current or endurance result in the repository.' (now tracked)
- `CHANGELOG.md:27` 0.99.99: 'the battery checks have not been run'
- `docs/P4X_EYE_PERIPHERALS.md:274` 'Not yet qualified: ... battery-only operation and power draw, idle/sleep clocks'
- `docs/WAVESHARE_P4_SUPPORT.md:20` Waveshare: 'Battery ADC support is enabled; only USB-powered/no-cell behavior is tested' (also 320-336)
- `components/hardwareone/System_Utils.cpp:1401` `voltage` mA figures remain a hard-coded estimate table
- `components/hardwareone/System_Battery.cpp:461` batterycalibrate: 'Divider accuracy still requires comparison with a meter.'
- `components/hardwareone/System_Power.cpp:284` 'No battery-driven mode switch exists in this firmware'
- ... 2 more in verify/D2.json

**Newer commits.** 64713c0d: tracked the power-policy/battery-budget docs; a18efd58: enabled Waveshare battery voltage sensing (another unvalidated ADC backend)

**Waveshare.** Adds a battery-ADC board (GPIO20 divider) tested only with no cell. The board is also absent from the README board table, so any README power paragraph should cover it or exclude it.

**Bounded fix.** Add one tracked paragraph (README Supported Hardware or docs/POWER.md, linked from the battery rows). It should say that runtime and current have not been measured, that the percentage is an estimate on ADC boards, that power modes change the CPU clock only, and that the device notifies at critical charge and runs until the cell protection or brownout. Add the matching USERGUIDE sentence. Later, take one meter comparison and one discharge per battery board.

**Done when.** git ls-files lists the paragraph's file; README battery rows link to it; it contains 'runtime has not been measured' (or equivalent) until a tracked docs/POWER_MEASUREMENTS.md supplies per-board dated rows (cell, idle mA, runtime, cutoff voltage, reset reason) and meter-vs-reported volts within 50 mV.

**Hardware needed.** For the later measurement: a known LiPo, a multimeter and a PPK2/INA226 per battery board (FeatherS3, Feather V2, P4X-EYE, Waveshare); one idle-current baseline and one full discharge with batterylog on.


## G-48 (P1, confirmed): Clock owner skips IDF's external-memory speed sync at P4 100/200 MHz and S3 UltraSaver 40 MHz

**Claim at 95dc3f34.** powerSetCpuFrequencyMhz calls the Arduino setCpuFrequencyMhz, which calls rtc_clk_cpu_freq_set_config_fast. No esp_clk_utils_mspi_speed_mode_sync_before/after call runs anywhere in the firmware or the vendored Arduino. IDF 5.5.5 requires that sync on P4 whenever the CPU clock is below CONFIG_SPIRAM_SPEED, which covers P4X Adaptive 100 and Balanced 200 against 250 MHz PSRAM and Waveshare 100 against 200 MHz. On S3 it is required for the XTAL-sourced UltraSaver 40 MHz. Waveshare now refuses downclocks only while DSI scanout is active.

**Why this priority.** The P4X default (Adaptive 100 MHz, and Balanced 200 MHz) is below its 250 MHz PSRAM clock with no MSPI speed-mode sync, against Espressif's stated constraint; Waveshare already showed external-memory underruns on clock transitions, and a P4X PSRAM corruption is unexplained, so the bounded fix or a written decision is warranted before 1.0.

**Refuted or overstated.** Nothing refuted. The P4 bench has now run at the Adaptive default through several multi-hour runs (including 8 h 44 min) without a reported external-memory fault, which keeps this unconfirmed rather than disproven. The original evidence paths under <other-account-home> are foreign; the same code is at ~/esp/esp-idf (v5.5.5) on this Mac.

**Evidence.**
- `components/hardwareone/System_Power.cpp:83` setCpuFrequencyMhz(mhz) with only I2C draining around it; line 44 adds the Waveshare displayRequiresFullSpeed refusal
- `components/arduino/cores/esp32/esp32-hal-cpu.c:283` rtc_clk_cpu_freq_set_config_fast(&conf); git grep finds no mspi_speed_mode_sync/esp_clk_utils in components/hardwareone or components/arduino
- `~/esp/esp-idf/components/esp_hw_support/clk_utils.c:22` v5.5.5: P4 BELOW_FREQ_THRESHOLD(freq) = freq < CONFIG_SPIRAM_SPEED; before-switch switches PSRAM to low-speed mode 'to avoid writing MSPI FIFO overflow' (37-44); S3 must sync when the CPU source is XTAL (29-35)
- `config/sdkconfig.defaults.esp32p4:23` CONFIG_SPIRAM_SPEED_250M=y; build-config-p4-phone-pipe/sdkconfig:1924 SPIRAM_SPEED=250 and 1902 PM_ENABLE unset; Waveshare build 1927 SPIRAM_SPEED=200
- `components/hardwareone/System_Power.h:24` POWER_MODE_DEFAULT = Adaptive; P4 floor 100 / Balanced 200 (36-37); POWER_CPU_ULTRA_IDLE_MHZ 40 (47)
- `docs/WAVESHARE_P4_INTEGRATION.md:186` 'LCD DSI external-memory-fetch underrun warning' right after powersleep downclock, absent in Locked mode (196-201); led to 138c59fb
- `docs/investigations/2026-11-04-p4x-fix-progress.md:855` Unattributed CORRUPT HEAP in a PSRAM block on P4X (2026-11-06). Not linked to clocking, but unsynced MSPI writes remain an open candidate.

**Newer commits.** 138c59fb: Waveshare refuses CPU clocks below Performance while DSI scanout runs (a symptom-level guard after observed underruns; does not add the MSPI sync and does not cover the P4X or display-off states)

**Waveshare.** It changes the picture: Waveshare PSRAM runs at 200 MHz, so Adaptive's 100 MHz is still below the threshold whenever scanout is inactive. Its observed DSI external-memory underrun on downclock is the first hardware symptom consistent with this constraint, and 138c59fb works around it only while the panel is lit.

**Bounded fix.** In powerSetCpuFrequencyMhz, bracket setCpuFrequencyMhz with esp_clk_utils_mspi_speed_mode_sync_before_cpu_freq_switching(src, target) and the _after_ variant (esp_private/esp_clk_utils.h; no-ops on ESP32 classic). Add a source-contract assertion in test_power_cpu_frequency.py. If the low-speed PSRAM mode costs too much at 100 MHz, raise the P4 floor to >= CONFIG_SPIRAM_SPEED instead and record the decision.

**Done when.** test_power_cpu_frequency.py asserts both sync calls (or a dated decision note explains their absence), and a dated bench note records >= 30 min on the P4X at 100 MHz idle and on an S3 at UltraSaver 40 MHz idle, each with a PSRAM write/verify loop, zero mismatches and no new crash records.

**Hardware needed.** P4X-EYE (and ideally Waveshare with the display asleep) at Adaptive idle, plus a FeatherS3/XIAO S3 in UltraSaver, each running a PSRAM write/verify pattern across repeated clock transitions for >= 30 min.


## G-50 (P1, confirmed): Stored XSS via filenames in the shared web file explorer (authenticated user-to-admin RCE through /api/cli)

**Claim at 95dc3f34.** FileBrowser.rowHtml inserts o.name raw into HTML, and createFileExplorer splices itemPath (currentPath + file.name) unescaped into single-quoted onclick JS, then sets listDiv.innerHTML. BondFs.esc only escapes backslash and single quote, then places the result in a double-quoted HTML attribute, and passes en.name raw to rowHtml. normalizeFsPath rejects only '..', '"' and control bytes. The catch-all rule gives a plain user PERM_ALL (including import) outside the listed prefixes, and the upload handler gates only on canImport. The Files, CLI, Logging, Maps and Bond pages all render through this code. There is no page CSP and no host test of these renderers.

**Why this priority.** Any authenticated user (or a paired ESP-NOW peer, or a bonded peer's FS_LIST reply) can plant a filename that runs script in the next admin's session on the device origin, a real privilege escalation on a multi-user web UI. The escaping fix is small.

**Refuted or overstated.** The /espnow/received inbox is admin-tier only (System_Filesystem.cpp:1667-1668), so the ESP-NOW vector needs a securely paired peer, not any peer. SD (FAT) paths reject '<' and '>' at the filesystem, so the stored payload works on LittleFS paths. LittleFS also splits on '/', so a payload must avoid '/' (easy, e.g. via atob). These constrain the attack but do not stop it.

**Evidence.**
- `components/hardwareone/WebServer_Utils.h:343` rowHtml: `' onclick="' + o.clickExpr + '"'` (342) and `'<span>' + o.name + '</span>'` (343).
- `components/hardwareone/WebServer_Utils.h:458` itemPath = currentPath + '/' + file.name; used raw in Navigate/Select/Download/Edit/Rename/Delete onclick strings (466-507); listDiv.innerHTML = html (527).
- `components/hardwareone/WebServer_Utils.h:318` breadcrumbHtml also inserts the raw path parts as HTML text.
- `components/hardwareone/WebServer_Utils.h:984` BondFs esc(): JS-string escaping only; used inside double-quoted onclick attributes (1139-1176); en.name is passed raw to rowHtml (1186); c.innerHTML = h (1208).
- `components/hardwareone/System_Filesystem.cpp:1764` normalizeFsPath rejects only '"' and bytes < 0x20 (plus '..' at 1759); '<', '>', ''' and '&' are accepted.
- `components/hardwareone/System_Filesystem.cpp:1682` Catch-all {nullptr, PERM_ALL (user), PERM_ALL, PERM_ALL}.
- `components/hardwareone/WebServer_Server.cpp:2325` handleFileUpload: tgRequireAuth, then only canImport(path, ctx) (normalize, perms, sensitive extension).
- `components/hardwareone/System_ESPNow.cpp:5089` The peer-supplied filename is joined verbatim into /espnow/received/<tok>/<filename> (also 4868 for streamed files). FILE_START requires REQ_PAIRED + REQ_SESSION_ENC (6092). No character filter in fileSlotsAllocate.
- ... 2 more in verify/C.json

**Newer commits.** 64713c0d: the only commit since v0.99.99 touching WebServer_Utils.h/System_Filesystem.cpp/WebServer_Server.cpp; the rendering code is unchanged

**Waveshare.** Applies unchanged: the Waveshare profile enables HTTP (77fcbdcf) and the shared explorer. Its SDMMC slot is FAT, which forbids '<' and '>', but internal LittleFS paths remain exploitable.

**Bounded fix.** In WebServer_Utils.h add one HTML-escape helper and apply it to o.name and the breadcrumb parts. Pass paths to handlers through data-path attributes read with this.dataset (as WebPage_Settings.h:2764 already does with encodeURIComponent) instead of building JS strings, in both createFileExplorer and BondFs. Optionally also reject '<', '>' and ''' in normalizeFsPath segments.

**Done when.** A host text test (style of test_espnow_page_render.py) asserts that rowHtml/breadcrumbHtml escape names and that no `'\'' + itemPath + '\''` or `'+full+'` concatenation remains in WebServer_Utils.h. Manual: as a user-role account, upload a file named `<img src=x onerror=alert(1)>.txt` to /. An admin opening Files sees the literal name, no dialog, and the row still opens.

**Hardware needed.** One browser check on any HTTP-enabled board: user-role upload of the hostile filename, then an admin views the Files page.


## G-51 (P1, confirmed): Wi-Fi scan renders beacon SSIDs raw into innerHTML on the admin Settings page (zero-credential radio-range XSS)

**Claim at 95dc3f34.** The Settings page runs `wifiscan json` and builds each result row with `'<div style="font-weight:bold">' + ssid + ...` from ap.ssid, then sets container.innerHTML. Only the Select button's data-ssid uses encodeURIComponent. Server-side, the scan JSON is built with ArduinoJson from the raw SSID bytes (JSON-safe, not HTML-safe). There is no page CSP. A 32-byte SSID is enough for an `<svg onload=...>` payload, for example one that imports an external script.

**Why this priority.** Zero-credential script execution in an admin session from anyone in Wi-Fi range when the admin presses Scan; the cost is one escape call.

**Refuted or overstated.** 'No escaper exists in the web runtime' is overstated: page-local escapeHtml/escHtml helpers exist in WebPage_Maps.h:2040, WebPage_Logging.h and WebPage_ESPNow.h:1471, but there is no shared one and Settings uses none. Exploitation needs an admin (or any user who opens the scan) to press Scan.

**Evidence.**
- `components/hardwareone/WebPage_Settings.h:2764` `... + ssid + ' ' + badge + '</div>'`, where ssid = ap.ssid (2752); esc = encodeURIComponent(ssid) is used only for data-ssid.
- `components/hardwareone/WebPage_Settings.h:2769` container.innerHTML = html.
- `components/hardwareone/WebPage_Settings.h:2713` Data source: postSettingsCli('wifiscan json').
- `components/hardwareone/System_WiFi.cpp:1501` cmd_wifiscan JSON path. appendWifiScanJson copies record.ssid verbatim into object["ssid"]. wifiscan is not admin-only (2697).
- `components/hardwareone/WebServer_Server.cpp:1835` No CSP on UI pages.

**Waveshare.** Applies to Waveshare as to every Wi-Fi + HTTP profile; the scan results come over ESP-Hosted from the C6 but reach the same renderer.

**Bounded fix.** Wrap ssid in an HTML escaper at WebPage_Settings.h:2764 (and the label text at 2810 if it is ever rendered as HTML), preferably via a shared hw.escHtml in the web runtime.

**Done when.** A host text test asserts the scan renderer escapes ssid before concatenation. Manual: a hotspot named `<b>x</b>` shows literally in the scan list.

**Hardware needed.** One check with a phone hotspot named `<b>x</b>` and a Settings > Wi-Fi > Scan on any board.


## G-52 (P1, confirmed): First-time setup accepts a reserved owner name and boots into permanent lockout

**Claim at 95dc3f34.** createInitialAdminUser rejects only an empty username or password, then writes role 'superadmin'. Neither the restore-flow prompt (383-398) nor the main wizard prompt (799-819) calls isValidPublicUsername or isValidPublicPassword. isValidUser and getUserIdByUsername refuse reserved names, so an owner named system/AuthBypass/bond-admin (case-insensitive) can never log in, while users.json exists and marks setup complete. The same gap admits passwords shorter than the 6-character public minimum.

**Why this priority.** A new owner who types 'system' or 'AuthBypass' at first setup bricks access to their device (recovery = erase and reflash); unlikely but a supported install path, and the fix is a call to an existing validator.

**Refuted or overstated.** The 'over-long name locks out' variant is overstated: web login (WebServer_Server.cpp:3736-3766) passes the full username to isValidUser, which has no length or charset check, so long or odd-character names can still log in at least on the web. Only the reserved-name case is a certain lockout. The 'sentinel becomes super-admin on an auth-off transport' variant needs auth to be turned off first, which this owner cannot do, so it is theoretical (see G-55).

**Evidence.**
- `components/hardwareone/System_FirstTimeSetup.cpp:280` Only blank checks; role = "superadmin" at 297.
- `components/hardwareone/System_FirstTimeSetup.cpp:385` Restore-flow username loop: only while empty (OLED input capped at 32 chars).
- `components/hardwareone/System_FirstTimeSetup.cpp:801` Main wizard username loop: only rejects blank and 'b'/'back'.
- `components/hardwareone/System_User.cpp:2227` isReservedUsername: system, AuthBypass, kBondAdminUser, case-insensitive after trim.
- `components/hardwareone/System_User.cpp:2778` isValidUser returns false for reserved names.
- `components/hardwareone/System_User.cpp:2834` getUserIdByUsername returns false for reserved names.
- `components/hardwareone/test/host/test_initial_admin_setup.py:None` grep reserved|invalid|AuthBypass|validate: no hits.

**Waveshare.** Same setup code; the Waveshare touch UI drives the same prompts.

**Bounded fix.** In createInitialAdminUser call isValidPublicUsername(username, &err) and isValidPublicPassword(password, &err), print err and return false. Make both prompt loops re-ask on failure instead of only on blank.

**Done when.** test_initial_admin_setup.py gains cases where createInitialAdminUser("AuthBypass", "secret12"), ("System", ...) and ("owner", "abc") return false and leave no users.json. Manual: the serial wizard rejects 'AuthBypass' and re-prompts.


## G-56 (P1, confirmed): 24 bad-argument replies return bare Usage: text and are classified as success by the OK:/Error: contract

**Claim at 95dc3f34.** 25 production reply sites return bare 'Usage: ...' text, one more than at v0.99.99 because the Waveshare speakertone command adds one. System_FeatureRegistry.cpp:930 also returns an unprefixed 'Unknown feature.'. The executor counts anything not starting with Error/ERROR as success, stamps 'OK: ' on it and logs it as succeeded, and the web CLI answers 200.

**Why this priority.** Reporting HTTP 200 'OK: Usage: ...' and a successful step for a rejected command breaks the documented reply contract that automations, the web CLI and the display/glasses feedback rely on, and the fix is mechanical.

**Refuted or overstated.** Nothing refuted. The count is now 25 Usage: sites plus 'Unknown feature.' (the original said 24 plus 1). The addition is the Waveshare speakertone reply, and the c6console site now spells c6_console.

**Evidence.**
- `components/hardwareone/System_Utils.cpp:4682` registrySuccess = strncmp(out,"Error",5)!=0 && strncmp(out,"ERROR",5)!=0. Then logCommandExecutionPrepared(..., registrySuccess) and stampOkStatus (4686), which prepends 'OK: ' (4250-4270).
- `components/hardwareone/WebServer_Server.cpp:3533` 400 only when !ok or the text starts with 'Empty command'/'Unknown command'.
- `components/hardwareone/System_RadioCompanion.cpp:989` c6hold; also 1049 'Usage: [on|off]' with no command name, 1066 c6_console, 1097 c6heartbeat.
- `components/hardwareone/System_Cm5HostControl.cpp:897` 8 sites: 897, 955, 1084, 1093, 1251, 1269, 1282, 1389.
- `components/hardwareone/System_WiFi.cpp:953` openwifi 953, 960, 969 (snprintf), and radiopower 1389.
- `components/hardwareone/G2_Glasses.cpp:29727` g2conversate 29727; g2terminal 29760 and 29782.
- `components/hardwareone/System_Speaker.cpp:12` NEW, Waveshare speaker command: 'Usage: speakertone ...' on more than 3 args, while its other refusals in the same function use Error:.
- `components/hardwareone/BLE_CoexRecords.cpp:267` blerecords
- ... 7 more in verify/E2.json

**Newer commits.** 3e5d4579: added System_Speaker.cpp with a new bare Usage: reply; 64713c0d: changed the c6console Usage text to c6_console

**Waveshare.** Adds one site (speakertone, System_Speaker.cpp:12, compiled under ENABLE_SPEAKER on the Waveshare board).

**Bounded fix.** Prefix the 25 replies with 'Error: ' (e.g. 'Error: invalid arguments. Usage: ...'), give System_RadioCompanion.cpp:1049 its command name, and prefix FeatureRegistry.cpp:930. Update the two pinned wifi test strings. Add a host source guard next to test_command_resolution_contract.py that fails on `return "Usage:` or `snprintf(..., "Usage:` in components/hardwareone/*.cpp outside test/.

**Done when.** git grep -nE 'return "Usage:|printf\(.*"Usage:' -- 'components/hardwareone/*.cpp' ':!components/hardwareone/test/*' returns 0 lines. The new guard is registered in CTest and passes. On a web-enabled build POST /api/cli 'c6hold maybe' returns 400 with an Error: body (host web test, or one bench curl).

**Hardware needed.** Optional: one curl of /api/cli with a bad argument on any web-enabled bench board to confirm the 400; the source guard settles the rest.


## G-58 (P1, confirmed): QUICKSTART first-boot wizard table predates the 2026-10-10 regroup and Basic-mode advice names the wrong step

**Claim at 95dc3f34.** QUICKSTART Step 3 still describes the pre-2026-10-10 wizard. It lists nine pages (Features, Web Mode, Bluetooth Mode, Sensors & Display, Startup & Auto-start, System Settings, ESP-NOW Identity, MQTT Broker, WiFi Setup), three of which 'always appear'. The firmware's order and titles are Connections, WiFi Setup, Web Mode, Bluetooth Mode, ESP-NOW Identity, MQTT Broker, Display & Controls, Sensors, System, and the group pages appear only when they have rows. Step 4 tells a Basic-mode user to run openhttp or httpAutoStart 1, but Basic already sets httpAutoStart and configures no WiFi, and HTTP starts only when WiFi is connected.

**Why this priority.** QUICKSTART first boot is the first thing every new 1.0 user follows: a wrong page table and Basic-mode advice that cannot bring the web UI up undermine trust at the very start.

**Refuted or overstated.** Nothing in the defect is refuted. One correction to the original proposed fix: 'run wifiscan then wifiadd, after which the web server starts on boot' is itself incomplete. Basic mode leaves wifiAutoStart=0 and wifiadd does not set it, so the rewrite must also say `wifiautostart 1` (or `openwifi` and then `openhttp` for the current session).

**Evidence.**
- `docs/QUICKSTART.md:238` Lines 238-253: the 'up to nine pages. Three always appear' text and the stale table (row 1 'Features', row 5 'Startup & Auto-start', row 9 'WiFi Setup').
- `components/hardwareone/System_SetupWizard.cpp:179` kPageOrder 179-189: CONNECTIONS, WIFI, WEBMODE, BTMODE, ESPNOW, MQTT, DISPLAY, SENSORS, SYSTEM.
- `components/hardwareone/System_SetupWizard.cpp:391` getWizardPageTitle 391-404 gives the real titles. The 'SETUP %d/%d: %s' banner is at 1037.
- `components/hardwareone/System_SetupWizard.cpp:848` wizardIsPageVisible: group pages are visible iff getWizardMenuItemCount(page)>0, so no page is unconditional.
- `docs/QUICKSTART.md:270` Step 4 Basic-mode advice: `openhttp` or `httpAutoStart 1`.
- `components/hardwareone/System_FirstTimeSetup.cpp:875` Basic sets wifiAutoStart=true and httpAutoStart=true (875-876). Then 914-916 clear wifiAutoStart because WiFi was never configured.
- `components/hardwareone/HardwareOne.cpp:1450` bootAutostartHttp starts HTTP only if WiFi.isConnected(). The boot WiFi connect is gated on wifiAutoStart (2187).
- `components/hardwareone/network/wifi/System_WiFiNetworks.cpp:296` Only the setup path (saveSetupWiFiNetwork, called from the wizard and FirstTimeSetup) turns wifiAutoStart back on. The plain `wifiadd` CLI does not, so a Basic user also needs `wifiautostart 1` (registered, COMMAND_REFERENCE.md:1468) for the web UI to come up on boot.

**Newer commits.** 365122c0 / 4b3c0550: touched System_SetupWizard/FirstTimeSetup for the GT911 touch input and the Waveshare merge; page order and titles unchanged

**Waveshare.** No change to page order or titles. The Display & Controls page gains touch-related rows on the Waveshare build (365122c0). QUICKSTART's 'serial and OLED simultaneously' framing also does not describe the Waveshare LCD.

**Bounded fix.** Rewrite QUICKSTART 236-261 from kPageOrder/getWizardPageTitle with the group-visibility rule, moving auto-start into the Connections and Sensors descriptions and the device name into System. Rewrite Step 4 for Basic mode as `wifiscan`, `wifiadd <ssid> <password>`, `wifiautostart 1`, then reboot (or `openwifi` and `openhttp` now). Add a host doc test asserting the table's page titles equal getWizardPageTitle strings in kPageOrder order.

**Done when.** The new doc test passes and fails if a title or the order is changed. Every command named in Step 4 is in COMMAND_REFERENCE.md. A Basic-mode run that follows Step 4 reaches the web UI after a reboot.

**Hardware needed.** One erase-and-flash first boot on any WiFi board: (a) an Advanced-mode serial transcript whose SETUP n/N titles match the table; (b) a Basic-mode run following the new Step 4 reaching the web UI after reboot.


## G-59 (P1, confirmed): USERGUIDE documents commands that do not resolve, removed features, stale auto-pair wording and wrong privilege/hardware names

**Claim at 95dc3f34.** I resolved commands with the firmware's whole-word, case-insensitive, longest-prefix rule against the CommandEntry registry parsed at 95dc3f34. These USERGUIDE commands do not resolve: oledrequireauth (419, 1205); the whole `mqtt broker|port|user|pass|topic|connect|disconnect|status` block (759-766); mqttPublishGamepad (1133, now mqttPublishInput); debuggamepad (937, now debuginput); cameraautocapture, cameraautocaptureinterval, camerasendaftercapture and cameratargetdevice (1524-1527); `espnow sendfile` (572, now espnowsendfile); and `fmradio tune` (1894, now fmradiotune). No fmradio, espnow, mqtt or ota root command exists, and module names are accepted only as single words inside help mode. factoryreset is annotated (admin) at 988 but is super-admin. MIGRATION.md:66 `ota status` does not resolve either (otastatus). The other stale text is still present: the R1 single-profile claim, the DISPLAY_TYPE/INPUT_DEVICE_TYPE ranges, the board reference without the P4 boards, TEA5767/QT PY in README, and peer_paired 'auto-pair'.

**Why this priority.** The USERGUIDE is the 1.0 command manual. 18 lines that return 'Unknown command' and a misstated privilege make users doubt the device rather than the doc, and the fix plus a registry-backed doc test is small.

**Refuted or overstated.** The CONN-10 member's untracked-design-doc part is mostly fixed by 64713c0d: BLE_SETUP_WORKER_PLAN, WIFI_BLE_COEXISTENCE_STUDY, PHONE_CONSOLE_VS_PIPE and PHONE_PIPE_HANDOFF are now tracked. Only ESPNOW_MESH_SYSTEM.md and ESPNOW_FILE_STREAMING_PHASE_A.md remain missing (see G-22). The c6console part belongs to G-21. The count is 18 unresolved USERGUIDE lines (13 distinct names) plus the factoryreset privilege. The remaining code-block misses are expected: automation IF/NAME/SCHEDULE syntax, debug<flagname> placeholders, the UART-only dictate line and prose continuations.

**Evidence.**
- `docs/USERGUIDE.md:419` oledrequireauth, also at 1205. Not registered; displayrequireauth is (COMMAND_REFERENCE.md:1092, S).
- `docs/USERGUIDE.md:759` Lines 759-766: eight `mqtt <verb>` lines. There is no `mqtt` command, so each is Unknown command. The real names are listed at 1111-1135.
- `docs/USERGUIDE.md:1133` mqttPublishGamepad is not registered; mqttPublishInput is (COMMAND_REFERENCE.md:306).
- `docs/USERGUIDE.md:937` debuggamepad is not registered; debuginput is (COMMAND_REFERENCE.md:983).
- `docs/USERGUIDE.md:1524` Lines 1524-1527: four camera auto-capture commands, none registered.
- `docs/USERGUIDE.md:572` `espnow sendfile` has no `espnow` root; the command is espnowsendfile (COMMAND_REFERENCE.md:237).
- `docs/USERGUIDE.md:1894` `fmradio tune <MHz>` has no `fmradio` root; the command is fmradiotune (COMMAND_REFERENCE.md:632).
- `docs/USERGUIDE.md:988` 'factoryreset ... (admin)', but COMMAND_REFERENCE.md:97 flags it S (super admin).
- ... 9 more in verify/E2.json

**Newer commits.** 64713c0d: tracked the design docs cited by CONN-10; changed only 2 lines of USERGUIDE.md (event rename); 3a3e3ba0 / 365122c0: added DISPLAY_TYPE 5 and INPUT_DEVICE_TYPE 4, widening the USERGUIDE build-flag gap

**Waveshare.** It widens the gap: USERGUIDE's DISPLAY_TYPE/INPUT_DEVICE_TYPE table and Board Reference now miss two P4 boards (P4X-EYE and Waveshare P4 Touch LCD 5), DISPLAY_TYPE 4/5 and INPUT_DEVICE_TYPE 3/4.

**Bounded fix.** Fix the 18 lines: oledrequireauth to displayrequireauth; the mqtt block to openmqtt/mqttHost/...; mqttPublishInput; debuginput; delete the camera auto-capture block; espnowsendfile; fmradiotune. Change 988 to (super admin) and MIGRATION.md:66 to otastatus. List the three R1 profiles, add DISPLAY_TYPE 4/5 and INPUT_DEVICE_TYPE 3/4 and the P4 board rows, use RDA5807, drop QT PY ESP32-S3, and correct the peer_paired row. Add a host test that resolves each code-block line of USERGUIDE.md with the System_CommandLookupCore rule against tools/command_registry.py parse_commands(), with an allow-list for IF/NAME/SCHEDULE, debug<flagname> and dictate.

**Done when.** The new USERGUIDE resolution test is registered in CTest, passes, and fails when an unregistered command is added to a code block. git grep -n 'TEA5767\|QT PY ESP32-S3' README.md docs is empty. USERGUIDE.md:228 names the same profiles as System_R1_Protocol.cpp:133-139. The (admin)/(super admin) annotations agree with the registry S/A column.


## G-60 (P1, confirmed): P4 local-STT profile needs a runtime model file no tracked artifact or URL provides

**Claim at 95dc3f34.** At 95dc3f34 local STT on P4X-EYE and Waveshare expects a .p4.stt file (and speaker.spk for labels) whose identity is pinned in stt_model_identity*.h, and no tracked file, release asset or URL provides it; the Waveshare record states the user supplied the model by hand.

**Why this priority.** Local STT is a headline P4 feature enabled by default in the board, deployment and saved profiles, but a cloner has no way to obtain the sha256-pinned model, so the feature cannot work outside the author's bench unless the model is published or the feature is labelled unavailable.

**Evidence.**
- `components/hardwareone/stt/stt_model_selection.h:8` HW1_STT_MODEL_PATH /sd/STT Models/quartznet15x5-gen2-25397.p4.stt (15x5 gen2) or /STT Models/quartznet5x5.p4.stt (line 13).
- `components/hardwareone/System_STTLocal.cpp:303` Readiness refuses with 'Install the matching model at ...'; size checked against the pinned identity (307).
- `components/hardwareone/System_STTLocal.cpp:360` Speaker labels need /sd/STT Models/speaker.spk; boards/p4x_eye.features.h:104-105 enables labels by default.
- `deployments/handheld/boards/p4x_eye/features.h:116` The handheld deployment enables ENABLE_LOCAL_STT 1.
- `configurations/waveshare-p4-bringup.json:16` Waveshare profile enables ENABLE_LOCAL_STT and ENABLE_STT_MODEL_15X5_GEN2 (line 22).
- `docs/WAVESHARE_P4_SUPPORT.md:444` 'The user supplied the matching 20,112,224-byte model' - pins the SHA-256 but gives no source.
- `docs/STT_LIVE_TEXT_TEST_STATUS.md:83` Pins the model path and SHA-256, no download location.
- `experiments/stt_p4/README.md:357` Model is produced by the training export; git grep finds no 'releases/download' or model URL in any tracked .md.

**Newer commits.** d2a6e64e / f718b692 / f11a05d8: Waveshare local STT enabled and verified with a hand-supplied model; readiness messages improved but no provisioning source added

**Waveshare.** Adds a second board whose saved profile depends on the same unpublished 15x5 gen2 model.

**Bounded fix.** Publish the 15x5 gen2 .p4.stt (and speaker.spk, and the 5x5 if the board default still selects it) as release assets or in the public STT repo, and add URL + sha256 to docs/P4X_EYE_PERIPHERALS.md and deployments/handheld/README.md; otherwise label local STT 'requires a model not yet published' in README and the matrix.

**Done when.** A tracked doc gives a download URL and sha256 for every path in stt_model_selection.h and speaker.spk, and the downloaded file's sha256 equals the pinned identity header value.

**Hardware needed.** After publishing: download the asset on a clean machine, copy to the SD path on one P4, confirm the readiness check passes and one transcription runs.

**Status 2026-11-08 (claude/stt-1.0).** The 15x5 gen2 model is published (README "Local speech-to-text model": URL and SHA-256). The unpublished 5x5 path is no longer selectable on a P4: `System_BuildConfig.h` refuses `ENABLE_LOCAL_STT` on ESP32-P4 without `ENABLE_STT_MODEL_15X5_GEN2`, which already requires the card; the plain `p4x_eye` profile and the handheld deployment (card off) build without local STT, and every saved P4 configuration sets `ENABLE_SDMMC_CARD` and `ENABLE_LOCAL_STT` (the model choice follows local STT; speaker labels are off without it). Host tests: `test_optional_peripheral_config.py`, `test_stt_model_selection.py`. Still open: `speaker.spk` has no published file (labels are skipped without it), and the on-device "download and install on a clean machine" check.


## G-61 (P1, confirmed): Docs claim boards/xiao_s3.defaults turns Bluetooth off; presets turn Bluedroid on for every board

**Claim at 95dc3f34.** QUICKSTART and the pocket_assistant README still say boards/xiao_s3.defaults sets CONFIG_BT_ENABLED=n; it does not, and presets/sdkconfig.defaults turns Bluedroid on for every board except the QT Py. Only the saved xiao-default profile turns it off, xiao-default.json and xiao-no-bluetooth.json are byte-identical, and the plain xiao_s3 build that README:208 uses is the same selection that CHANGELOG 0.99.98.5 recorded as over its partition by 31 KB.

**Why this priority.** The docs misstate how Bluetooth is selected on the XIAO, and README's one concrete build example (plain xiao_s3, Bluetooth on) very likely no longer fits its partition, so a newcomer's first command may fail.

**Refuted or overstated.** The qtpy_esp32 half of the QUICKSTART sentence is correct. Whether the plain xiao_s3 build overflows at 95dc3f34 is not proven (no build allowed here); a 2026-10-26 build with uncommitted changes reportedly had 3,456 bytes free, and 47 commits of shared code have landed since.

**Evidence.**
- `docs/QUICKSTART.md:48` Lines 48-50, table 177 'off', callout 181-187: 'Two boards ship with the Bluedroid stack compiled out'.
- `deployments/pocket_assistant/README.md:69` 'boards/xiao_s3.defaults sets CONFIG_BT_ENABLED=n'.
- `boards/xiao_s3.defaults:1` No CONFIG_BT_ENABLED or Bluetooth line (grep).
- `presets/sdkconfig.defaults:9` CONFIG_BT_ENABLED=y, reconciled on every build_board.sh build (lines 4-8); presets/qtpy_esp32.sdkconfig.defaults:3 is the only =n; selection logic in cmake/HW1Sdkconfig.cmake:5-7.
- `configurations/xiao-default.json:1` ENABLE_BLUETOOTH 0, CONFIG_BT_ENABLED false; diff with xiao-no-bluetooth.json is empty.
- `configurations/README.md:270` Measured table dated 2026-10-07 still shows xiao-default 5,883,136 B and xiao-no-bluetooth 4,350,144 B, i.e. as different selections.
- `CHANGELOG.md:138` 0.99.98.5: 'xiao-default no longer fits its partition on this tree (over by 31 KB)'; xiao-default at that time was {ENABLE_MQTT:0} (git show f694a01b~1), the same selection as the plain xiao_s3 board build (boards/xiao_s3.features.h:8-9 sets MQTT 0).
- `README.md:208` 'tools/build_board.sh xiao_s3 -p PORT flash monitor' is the only concrete board example.
- ... 1 more in verify/B.json

**Newer commits.** none touch boards/xiao_s3*, presets/ or the xiao configurations; shared-code growth since aa6508fd (crash records, BLE fixes, log rotation) can only shrink the plain build's margin

**Bounded fix.** Rewrite QUICKSTART 48-50/177/181-187 and pocket_assistant README:69 to say Bluetooth SDK policy lives in presets/ (shared on, QT Py off) and that the XIAO drops it only through a saved profile; change README:208 to 'python3 tools/build_config.py build xiao-default' or a board that fits; delete xiao-no-bluetooth.json or re-differentiate it, and refresh the measured table.

**Done when.** grep CONFIG_BT_ENABLED presets/*.defaults boards/*.defaults matches each doc's sentence; the build command README.md:208 names, built at the release commit, writes BUILD_INFO.md with positive app headroom; diff configurations/xiao-default.json configurations/xiao-no-bluetooth.json is non-empty or one file is gone.

**Hardware needed.** No hardware; one build of 'tools/build_board.sh xiao_s3' at the release commit settles whether README's example fits.


## G-15 (P2, confirmed): Shared-header defaults enable features every contract disables; on-device LLM compiled into classic-ESP32 images contrary to docs

**Claim at 95dc3f34.** System_BuildConfig.h still defaults HTTPS, MQTT, UART host link, bonded mode, LLM backend/onboard/CM5 and Maps on. Only xiao_s3 and the P4 board profiles override some of them, so bare FeatherS3, QT Py and Feather V2 builds get them all. The onboard LLM sources are gated by the flag alone, with no target or PSRAM condition, although USERGUIDE:59, QUICKSTART:100, README:113 and the header comment say S3 + PSRAM only.

**Why this priority.** The release artifacts are the deployment contracts, which already turn these features off. What remains is a misleading doc statement plus developer defaults, so it should be fixed but does not block a trustworthy 1.0.

**Refuted or overstated.** The cited build-qtpy_esp32/BUILD_INFO.md files are not in this checkout, so the classic-ESP32 resolution is inferred from the header and CMake rather than re-read from a manifest. The inference holds: no derived rule turns ONBOARD off on classic ESP32.

**Evidence.**
- `components/hardwareone/System_BuildConfig.h:438` LLM_BACKEND/ONBOARD/CM5 = 1 (438-440); BONDED 398; MQTT 118; HTTPS 114; UART_HOST_LINK 383; MAPS 451
- `components/hardwareone/System_BuildConfig.h:1027` 'requires ESP32-S3 with PSRAM. CMakeLists.txt handles excluding LLM source files on non-S3 targets' - no such rule exists
- `components/hardwareone/CMakeLists.txt:555` Onboard LLM sources appended on HW_CFG_ENABLE_LLM_SOURCE_ONBOARD alone; no IDF_TARGET test
- `docs/USERGUIDE.md:59` '(ESP32-S3 + PSRAM only)'; docs/QUICKSTART.md:100 and README.md:113 repeat it
- `boards/xiao_s3.features.h:8` Only xiao_s3 turns MQTT off; feathers3/qtpy_esp32/feather_esp32_v2 have no features.h

**Waveshare.** boards/waveshare_p4_touch_lcd_5.features.h overrides none of these, so a bare Waveshare board build also inherits LLM/bonded/MQTT/HTTPS on. Its saved profile waveshare-p4-bringup turns them off.

**Bounded fix.** Either add a derived rule (ENABLE_LLM_SOURCE_ONBOARD forced 0 unless CONFIG_IDF_TARGET_ESP32S3 or P4 with SPIRAM, mirrored in CMake) or reword the three docs and the header comment to the measured behaviour. Optionally set the shared defaults for LLM onboard, bonded and MQTT to 0 to match the contracts.

**Done when.** Docs and build agree: either a classic-ESP32 BUILD_INFO shows ENABLE_LLM_SOURCE_ONBOARD off, or USERGUIDE/QUICKSTART/README/header no longer claim S3-only.


## G-18 (P2, partly_confirmed): Three web-UI JavaScript tests fail at HEAD

**Claim at 95dc3f34.** The camera page test was fixed test-side in fc4a0cdf: strict-mode JS engines keep eval'd declarations local, so the page was fine and the test was stale. The battery log-table and Edge Impulse tests are unchanged since aa6508fd and their pages are unchanged too, so they presumably still fail. They don't call eval-declared names directly, so the cause may differ and is still untriaged.

**Why this priority.** The camera failure turned out to be a harness artifact. The remaining two are probably the same kind, and the battery log table and Edge Impulse overlay are not core 1.0 paths, so a quick triage is enough.

**Refuted or overstated.** For the camera page, 'pages dead in a real browser' is refuted: the defect was in the harness and is fixed.

**Evidence.**
- `tools/webui/tests/test_camera_page.py:143` fc4a0cdf runs the shipping script and assertions in one function scope ('Strict-mode engines keep eval declarations local to the eval'), with no production change
- `tools/webui/tests/test_battery_page.py:115` LOG_HARNESS eval's the page then calls the harness-captured fetchText callback; file unchanged since aa6508fd; WebPage_Battery.cpp unchanged since aa6508fd
- `tools/webui/tests/test_edgeimpulse_page.py:87` eval then window._eiShowDetections; test and System_EdgeImpulse_Web.h unchanged since aa6508fd
- `tools/webui/js_engine.py:6` This Mac has no node; deno/jsc/osascript fallbacks differ in strict-mode and output behaviour

**Newer commits.** fc4a0cdf: fixed the camera test harness scoping and added Waveshare OV5647 capability checks

**Waveshare.** fc4a0cdf extended the camera page test with a Waveshare OV5647 (p4-csi) capability case. The page now serves a third backend, so a passing camera test covers more boards.

**Bounded fix.** Run the battery and EI tests under node and deno. If they pass under node, apply the fc4a0cdf single-scope harness pattern; if not, fix the page.

**Done when.** python3 -m unittest discover -s tools -p 'test_*.py' reports 0 failures at the release commit under the engine CI uses, and a manual browser check of the battery log table is recorded.

**Hardware needed.** If the tests still fail under node: open the battery page on a device with a battery log and check that the table renders.


## G-20 (P2, confirmed): CMake source selection and System_BuildConfig.h guards are hand-mirrored for 17 feature families with one agreement test

**Claim at 95dc3f34.** Source selection is still mirrored by hand between component CMake and System_BuildConfig.h. Only the network/web family has a CMake-vs-preprocessor agreement test (test_network_build_selection.py), plus a literal-form check on the parsed flags. The Waveshare work added three more mirrored families (DISPLAY_TYPE 5, INPUT_DEVICE_TYPE 4/GT911, ENABLE_SPEAKER) with no agreement test. CMake still reads the first ENABLE_CAMERA_SENSOR literal (1, the XIAO-Sense branch) on non-XIAO builds without an override.

**Why this priority.** The dangerous direction (CMake drops a source the header enables) mostly fails loudly at link time, and the release profiles are fixed literals, so this is a maintainability and test gap rather than a release blocker.

**Refuted or overstated.** '17 families' and specific line numbers have drifted. The no -Wundef point stands (grep finds none). Impact is mostly loud link errors, not silent misbuilds.

**Evidence.**
- `components/hardwareone/CMakeLists.txt:96` HW_CFG_TEXT_FLAGS first-match parse (96-117) now includes ENABLE_SPEAKER; literal-form check exists (also at aa6508fd)
- `components/hardwareone/System_BuildConfig.h:210` ENABLE_CAMERA_SENSOR conditional default 1/0 (210-216); first regex match is the 1, which passes the literal check
- `components/hardwareone/CMakeLists.txt:412` DISPLAY_TYPE 5 -> HAL_Display_Waveshare.cpp; 394 INPUT 4 -> Input_GT911.cpp; 417 ENABLE_SPEAKER -> HAL_Speaker_Waveshare.cpp; mirrors System_BuildConfig.h:524, 670, 203
- `components/hardwareone/CMakeLists.txt:321` System_Lz4.cpp selected under the G2 gate; the file has no outer feature guard
- `components/hardwareone/test/host/test_network_build_selection.py:36` web_guards(): eight web TUs, 'app-specific gates are separate'; only CMake-agreement test
- `components/hardwareone/test/host/test_optional_peripheral_config.py:1` Checks header derivation only (c++ -fsyntax-only), not CMake source lists
- `CONTRIBUTING.md:196` 'preserve their agreement and cover both with a configuration test' - not done for non-web families

**Newer commits.** 3e5d4579: added DISPLAY_TYPE 5 and ENABLE_SPEAKER mirrors in CMake and header; 365122c0: added INPUT_DEVICE_TYPE 4 (GT911) mirror

**Waveshare.** Waveshare increased the mirrored surface by three families. Its profile sets ENABLE_CAMERA_SENSOR explicitly, so the camera first-match quirk does not affect it.

**Bounded fix.** Extend test_network_build_selection.py with a small matrix (G2, AUTOMATION, MAPS, LLM, LOCAL_STT, DISPLAY 0/4/5, INPUT 0-4, SPEAKER) asserting that every TU whose outer guard preprocesses true is in hardwareone_srcs. Give System_Lz4.cpp an outer guard.

**Done when.** New cases registered in test/host/CMakeLists.txt pass on HEAD and fail when any covered list(APPEND hardwareone_srcs ...) line is removed.


## G-22 (P2, partly_confirmed): Tracked docs, CHANGELOG and source comments reference documents a cloner cannot read; the ignore policy behind it is hidden

**Claim at 95dc3f34.** Checked against git ls-files at 95dc3f34, tracked Markdown (excluding hardwareone_libs, experiments and the ignored components/arduino) has 21 references a cloner cannot follow. Of these, 16 are missing or untracked, 3 point at /private/tmp and 2 at the ignored companion private/ clone. Tracked sources under components/hardwareone cite 28 distinct docs/*.md that are not in the tree, among them G2_PROTOCOL.md at 17 sites. The .gitignore docs/*AUDIT*/*_REVIEW* patterns still hide docs cited by CHANGELOG.md:205 and docs/R1_HEALTH_2_3_2_RESEARCH.md:645. No tracked link checker exists and RELEASING.md has no such step.

**Why this priority.** The user-facing docs (README, QUICKSTART, USERGUIDE, setup guides) resolve cleanly and 64713c0d tracked the eight most-cited design docs, so what is left are dead links in developer docs, old release notes and code comments: worth a cheap checker, not a release blocker.

**Refuted or overstated.** Largely superseded by 64713c0d, which tracked CROSS_BOARD_POWER_POLICY, PHONE_PIPE_HANDOFF, WIFI_BLE_COEXISTENCE_STUDY, BLE_SETUP_WORKER_PLAN, PHONE_CONSOLE_VS_PIPE, ANDROID_AGENT_PROMPT_CONSOLE_ON_PHONE_LINK and STT_VOCABULARY_AND_FORMATTING. So CHANGELOG:397, CHANGELOG:112, the host-suite README:12, tools/ble-phone-pipe/README and the System_BuildConfig WIFI_BLE study citation now resolve, and the 'eight untracked working-tree docs' finding is gone. The .git/info/exclude parts (TREE-8/TREE-11) describe another clone: this checkout's exclude holds only /work/ and /mechanical/, so they say nothing about the repository. docs/images.md is a reference into the g2-kit project ('ble/docs/images.md'), not this repo. README.md:139 ('XIAO has no OTA layout yet') is about the main firmware's xiao_s3 board, which indeed has no OTA layout; the hw1-pa-xiao-ota-v1 layout belongs to the separate pocket_assistant deployment, so 'contradicts the tree' is overstated (at most a missing pointer).

**Evidence.**
- `CHANGELOG.md:205` Cites docs/MEMORY_PRESSURE_AUDIT_2026_10_21.md; git check-ignore: .gitignore:53 docs/*AUDIT*.md.
- `CHANGELOG.md:2007` docs/BLE_SENSOR_CONTROLS_CONTRACT.md is not tracked (line 2387 does the same for docs/AUTH_LOG_FORMAT.md). These are old release entries.
- `tools/btsnoop/README.md:6` Lines 6-7 link docs/OTA_PASSIVE_CAPTURE.md and docs/OTA_RESEARCH_FINDINGS_2026-07-31.md, neither in the tree.
- `docs/FILESYSTEM_PSRAM_IMPLEMENTATION.md:92` Links to /private/tmp scratch READMEs at 92, 132 and 133.
- `physical_enclosures/p4x_eye/README.md:11` Links output/pdf/p4x-eye-r26-pcbway-assembly.pdf; output/ is ignored at .gitignore:114.
- `docs/arduino-local-patches/README.md:93` Says 'This docs/ directory is itself untracked', but 9 files are tracked. Line 23 cites docs/DRAM_LONGTAIL_VERDICTS_2026-08-19.md, which is not in the tree.
- `docs/R1_HEALTH_2_3_2_RESEARCH.md:645` NEW since v0.99.99 (64713c0d): cites docs/R1_HEALTH_2_3_2_REVIEW.md, ignored by .gitignore:56 docs/*_REVIEW.md.
- `docs/investigations/2026-11-04-p4x-fix-progress.md:1203` NEW (582f2c98): cites docs/SETTINGS_MATRIX.md, which is not in the tree. docs/ANDROID_AGENT_PROMPT_CONSOLE_ON_PHONE_LINK.md:148 cites docs/BLE_RELAY_PIPE.md, also not in the tree.
- ... 9 more in verify/E2.json

**Newer commits.** 64713c0d: tracked the eight previously untracked docs cited by tracked files; added R1_HEALTH_2_3_2_RESEARCH.md, which cites an ignored *_REVIEW doc; 582f2c98: added docs/investigations/2026-11-04-p4x-fix-progress.md with a dangling SETTINGS_MATRIX.md link; 4ed1eaa5: placeholder addresses in docs (shows a docs sweep ran, but not a link sweep)

**Waveshare.** The new Waveshare docs (WAVESHARE_P4_INTEGRATION.md, WAVESHARE_CAMERA_PLAN.md, LOCAL_WAVESHARE_MERGE.md) add no dangling links; the link walk found none in them.

**Bounded fix.** Commit a small tools/check_doc_links.py: relative links and docs/*.md mentions in git ls-files '*.md' (excluding hardwareone_libs, experiments) must resolve to tracked paths, with an allow-list for historical CHANGELOG entries. Fix the 21 hits: rewrite them as git show <tag>:path, delete the /private/tmp links, track or drop the enclosure PDF, and fix arduino-local-patches/README.md:93. Add the check to RELEASING.md. Batch-annotate or drop the 28 code-comment doc citations, or track G2_PROTOCOL.md if it can be published.

**Done when.** python3 tools/check_doc_links.py exits 0 in a fresh clone of the release commit, and RELEASING.md lists it as a pre-tag step.


## G-27 (P2, confirmed): Images are not byte-reproducible and BUILD_INFO provenance is too thin to audit a release

**Claim at 95dc3f34.** No reproducible-build setting is configured: config/sdkconfig.defaults* has no CONFIG_APP_REPRODUCIBLE_BUILD, and the IDF default CONFIG_APP_COMPILE_TIME_DATE=y embeds the build time. BUILD_INFO records only the short SHA and a dirty count, with no full SHA, IDF commit, Arduino patch digest, lock hash or image SHA-256. build_deployment.sh writes no SHA256SUMS. The signed OTA manifest carries imageSha256 but no source revision.

**Why this priority.** Byte-reproducibility is valuable but not expected of a first hobby-scale 1.0. A full SHA plus a SHA256SUMS file in the release directory are cheap provenance wins, and the OTA manifest already signs the image hash.

**Refuted or overstated.** 'Two builds differ' is inferred from configuration (compile date/time in esp_app_desc_t), not measured in the tree. It is very likely true. For OTA contracts the signed manifest already lets a user verify the image bytes, so only the source-to-image link is missing there.

**Evidence.**
- `config/sdkconfig:405` '# CONFIG_APP_REPRODUCIBLE_BUILD is not set'; 488 CONFIG_APP_COMPILE_TIME_DATE=y; no CONFIG_APP_REPRODUCIBLE key in config/, presets/, boards/ or deployments/
- `tools/gen_build_info.py:264` git rev-parse --short HEAD; dirty count only (264-268, 419-420)
- `tools/build_deployment.sh:183` Release dir gets images, BUILD_INFO, contract, partitions, MIGRATION; no checksum file
- `tools/ota/make_manifest.py:391` imageSha256 signed in the manifest; no source revision field

**Waveshare.** Waveshare has no release directory at all (no contract). Its only provenance is BUILD_INFO plus image SHA-256s written by hand in docs/WAVESHARE_P4_SUPPORT.md.

**Bounded fix.** Write the full SHA, IDF commit/dirty and image SHA-256 into BUILD_INFO. Have build_deployment.sh write SHA256SUMS. Optionally set CONFIG_APP_REPRODUCIBLE_BUILD=y in config/sdkconfig.defaults and check two builds match.

**Done when.** Release directory contains SHA256SUMS that `shasum -c` verifies and BUILD_INFO shows a 40-char commit; if reproducibility is enabled, two clean feathers3 builds of one commit give identical hardwareone-idf.bin hashes.


## G-30 (P2, partly_confirmed): No identity or partition-layout check at cable-flash time; app-only flash over an older table corrupts the data partition

**Claim at 95dc3f34.** Factory-only cable installs (P4X-EYE handheld deployment, the Waveshare profile and ordinary board builds) still have no tool that compares the device's partition table and board identity with the release before an application-only write; docs ask the operator to compare by hand. The port guard is the only flash-time check.

**Why this priority.** No public release ever shipped a P4 with the old table, so the concrete data-loss scenario is bench-only today; the missing flash-time check is a guard for the next layout change rather than a 1.0 blocker.

**Refuted or overstated.** The headline P4 scenario (a pre-2026-10-01 table on a P4) cannot occur on any tagged release: boards/p4x_eye.defaults and partitions_no_sr_16mb_p4x_eye.csv were added together in 5a1065e8 (v0.99.96), so only bench units ever carried the old table. Ordinary boards following QUICKSTART use 'flash', which rewrites the partition table, so they hit G-08's mount halt rather than silent corruption. The same-chip wrong-board risk is real but undemonstrated.

**Evidence.**
- `deployments/handheld/boards/p4x_eye/MIGRATION.md:30` 'Flash a new release (keeps user data)': esptool write-flash 0x10000 firmware.bin; 43-48 tell the user to compare tables themselves.
- `partitions/partitions_no_sr_16mb_p4x_eye.csv:13` Comment claims 'deploy tools compare the CSV before flashing' (lines 13-16); no such tool is tracked.
- `tools/ota/require_flash_port.py:1` The only pre-flash hook (CMakeLists.txt:874-905): it resolves the serial port; it does not read or compare the partition table.
- `docs/WAVESHARE_P4_SUPPORT.md:36` 'Do not flash an image whose resolved board, flash size or partition table differs'; lines 595, 652 and 686 record that the bench operator verified MAC/partition layout before application-only flashes - a manual step, no tracked tool.
- `partitions/partitions_no_sr_32mb_waveshare_p4_touch_lcd_5.csv:1` 'NOT compatible with the vendor demo layout ... First deployment requires backup and full layout migration'.

**Newer commits.** 3a3e3ba0: Waveshare adds another factory-only board whose application-only updates rely on manual layout verification

**Waveshare.** Adds a second factory-only P4 board and an explicitly incompatible vendor layout; its docs require a layout match before app-only flashing but provide no tool.

**Bounded fix.** Add a small tracked check (read the device table at 0x8000 with esptool, parse with gen_esp32part.py, compare to the release's partition-table.bin/BUILD_INFO) and call it from the P4 and Waveshare 'keeps user data' steps; until then, remove the 'deploy tools compare the CSV' sentence from the P4 CSV and the 'deploy tool' reference from MIGRATION.md.

**Done when.** The tool refuses with a named mismatch on a host unit test with two differing tables, and the P4 and Waveshare application-only flash instructions invoke it; or the false references are removed.

**Hardware needed.** One read-only run of the check against a connected P4X-EYE and Waveshare to confirm it reads and matches their tables.


## G-31 (P2, confirmed): Backup covers a fixed subset with no NVS, notifications policy or capture key, while USERGUIDE calls it the device's configuration

**Claim at 95dc3f34.** The .hwbackup still contains only settings, debug, users, boot anchors, user_settings/, automations, ESP-NOW devices, certs and maps (with users/certs skipped on a different device); /system/notifications.json and all NVS (recovery credential, capture sealing key) are outside it, and USERGUIDE calls it 'the device's configuration' with no coverage list.

**Why this priority.** The backup works for what it covers, but USERGUIDE implies it is the whole configuration; a coverage table and the notifications file are small fixes, and device-bound NVS secrets are a documented design choice rather than a defect.

**Refuted or overstated.** Original P1 overstated: excluding NVS secrets (recovery credential, capture key) is deliberate device binding, not a backup bug; the actionable defect is the missing coverage statement plus notifications.json. Captures themselves are not in the web backup at all, so 'sealed rows undecryptable after restore' applies only if the user copies logs separately.

**Evidence.**
- `components/hardwareone/WebServer_MigrationTool.cpp:439` Backup set (439-454); maps trimmed to the restore cap.
- `components/hardwareone/WebServer_MigrationTool.cpp:546` restorePathAllowed admits only that set; others refused at 619-623; cross-device restore also skips /system/users/ and cert files (626-633).
- `components/hardwareone/System_Notifications.cpp:182` NOTIF_POLICY_FILE /system/notifications.json not in the backup set.
- `components/hardwareone/System_OTA.cpp:688` Recovery credential (ap_pass/auth_token) in NVS; not backed up.
- `components/hardwareone/System_CaptureCrypto.cpp:23` Capture sealing key in NVS namespace hw1cap; not backed up.
- `tools/ota/device_backup.py:6` 'NVS is intentionally not touched'; /sd and /system/ota skipped (172-174).
- `docs/USERGUIDE.md:506` 'exports the device's configuration to a .hwbackup file'; 513-515 call it the reason a factory reset is not a catastrophe; no coverage list.
- `docs/COMMAND_REFERENCE.md:1114` capturecrypt export (plaintext copy) exists but its help does not mention that sealed captures do not survive a restore.

**Bounded fix.** Add a USERGUIDE 'what a backup includes' table (web .hwbackup vs tools/ota/device_backup.py vs NVS, device-bound items, size cap, cross-device skips); add /system/notifications.json to the backup set, restorePathAllowed and the migration_restore harness; mention in capturecrypt help that sealed files need export before migration.

**Done when.** USERGUIDE Backup & Restore lists included and excluded items; test_migration_restore.py restores notifications.json; capturecrypt help names the restore limitation.

**Hardware needed.** Optional: one export/import on a bench board confirming notification mutes survive.


## G-33 (P2, fixed_since_v0.99.99): G2 half-connected orphan never recovers at HEAD; the escalation exists only uncommitted and untested

**Claim at 95dc3f34.** In the committed tree, a run of 7 found-but-refused repair opens (gRepairOpenFailStreak, the length of one fast ladder) now admits bleStackRecycleIfWedged past the links-up refusal, posts SYSEVT_BLE_STACK_RECYCLED 'half-connected' and restores the peers that were live. Host harnesses cover it, but no dated hardware run shows the half-connected recycle firing, and CHANGELOG [Unreleased] does not mention it.

**Why this priority.** The escalation that was uncommitted at the tag is now in the tree with a host test, and recovery is bounded at about 13 minutes; what remains is one bench run, which can be folded into the G-34 range-loss session.

**Refuted or overstated.** 'Never recovers at HEAD' and 'the escalation exists only uncommitted' are no longer true: 64713c0d committed the escalation with host coverage. The untracked docs/G2_LEFT_TEMPLE_DISCONNECT_REVIEW.md cited as evidence was not carried into the tree.

**Evidence.**
- `components/hardwareone/G2_Glasses.cpp:2937` gRepairOpenFailStreak and kRepairOpenFailTrip = 7 (== g2repair::kFastTries) are in the committed tree
- `components/hardwareone/G2_Glasses.cpp:18748` a failed Repair-purpose open increments the streak; a successful open resets it (18760)
- `components/hardwareone/G2_Glasses.cpp:19265` halfStuck admits the recycle even while a temple, the ring or the phone is up (19299-19300); event subject 'half-connected' at 19319
- `components/hardwareone/G2_Glasses.cpp:14029` missingArmRecoveryComplete clears the streak when the episode ends
- `components/hardwareone/HardwareOne.cpp:2750` bleStackRecycleIfWedged runs every main-loop Bluetooth tick
- `components/hardwareone/test/host/ble_recycle_phone_harness.cpp:109` the harness asserts the half-connected recycle with the right temple and phone up (g2-deinit, phone-close, stack-down/up, g2-init), streak spent, and refusal at trip-1
- `components/hardwareone/G2_RepairCore.h:19` fast ladder 5+10+30+60+120+240+300 s, so the trip is reached after about 13 min, then a 600 s slow retry
- `docs/investigations/2026-11-04-p4x-fix-progress.md:718` 2026-11-05 hardware: L=down R=up repair=1/7 while the setup worker was wedged; the orphan path never ran because the worker was stuck (a different defect, now bounded by d0f050f9)
- ... 1 more in verify/E1.json

**Newer commits.** 64713c0d: absorbed gRepairOpenFailStreak, the halfStuck admission and the updated ble_recycle_phone/g2_repair_schedule harnesses; d0f050f9: an OPEN_TIMEOUT now retires the client (which can trip the mustRetireClient recycle path), and the 240/600 s overrun watch bounds a stuck setup worker that would otherwise block the repair, as on 2026-11-05; fbc6fecc: AUTO rediscovery of one admitted arm keeps the saved mate, so ring advStart is not stranded after a one-arm return; 582f2c98: investigation docs record the 2026-11-05 LEFT drop (HCI 0x02) as unexplained and the orphan recovery as not exercised

**Waveshare.** Applies equally: the Waveshare saved profile enables G2 glasses and the phone pipe, which run through the same shared G2_Glasses/BLE code over the C6.

**Bounded fix.** No code change needed. Run one bench check and record it in CHANGELOG.

**Done when.** A dated log shows a 'half-connected' SYSEVT_BLE_STACK_RECYCLED followed by L=up R=up and the ring and phone restored, and a CHANGELOG line names the board, image and outcome.

**Hardware needed.** P4X-EYE with glasses, ring and phone up: induce a host-orphaned temple (or the closest stand-in: keep one temple advertising but refusing opens) for about 15 min, confirm the streak reaches 7, the recycle runs once, and both temples, the ring and the phone return without a command.


## G-35 (P2, partly_confirmed): ESP-NOW multi-hop relay and channel-follow have no host harness or hardware record

**Claim at 95dc3f34.** System_ESPNow_Router.cpp is unchanged since the tag and still has no host harness. No three-node relay run is recorded; the only two-board ESP-NOW record explicitly does not cover multi-hop. USERGUIDE presents multi-hop with no experimental label. The two-device offline meeting is recorded (channel 6, no Wi-Fi, 2026-10-09); peers following a channel change are not.

**Why this priority.** Multi-hop is an advertised add-on beyond the recorded two-device path; a failure means a relayed message does not arrive, with no data or security exposure (pairing stays single-hop by design), so labelling it experimental or one bench run is enough for 1.0.

**Refuted or overstated.** 'No two-device channel-pin check' is overstated. Two boards meeting on channel 6 without Wi-Fi is recorded (2026-10-09), though the record does not say whether espnowchannel pinning was used. Peers following a change of channel remain unrecorded.

**Evidence.**
- `components/hardwareone/System_ESPNow_Router.cpp:1` 353 lines; no commit since aa6508fd; no host test or harness references it (grep of test/host)
- `components/hardwareone/test/host/CMakeLists.txt:1876` espnow_* suites: bond_json, deadline_wrap, directory, disabled_interface, dispatch_policy, fs_transcript_privacy, fslist_locking, message_store, metadata_push/text, page_render, radio_ownership, reassembly, rekey_retry, session_rekey/replay, tx_multiframe, user_credentials; none is a router suite
- `docs/XIAO_QTPY_CAMERA_ESPNOW_TEST.md:96` the 2026-10-09 record states it 'does not qualify ... multi-hop/range/interference'
- `docs/XIAO_QTPY_CAMERA_ESPNOW_TEST.md:69` refutation: 'Both radios used channel 6 without joining Wi-Fi' is a dated two-device offline meeting record
- `CHANGELOG.md:1764` 0.99.8 'ESP-NOW is now a real multi-hop mesh'; failover claims at 1781/1793; no bench line in the 0.99.8 section
- `CHANGELOG.md:1952` 0.99.4 peers follow the radio; no dated hardware line for a channel change
- `docs/USERGUIDE.md:529` the 'Multi-hop' section describes relaying as a normal feature with no experimental label

**Newer commits.** 64713c0d: ESP-NOW changes are comment renames only; the router is untouched; none of the six BLE commits is relevant

**Waveshare.** Does not apply: the Waveshare saved profile builds without ESP-NOW (NETWORK_FEATURE_LEVEL 2).

**Bounded fix.** Either label multi-hop and channel-follow 'experimental' in USERGUIDE and the 1.0 release matrix, or run a three-node relay check. In either case, add a small router host harness for route learning, expiry and hysteresis.

**Done when.** Either USERGUIDE and the release notes carry the experimental label, or a dated record exists with three-node espnowmeshroutes output, a routed espnowsend, and route expiry after B leaves. A router ctest exists and fails when route expiry is removed.

**Hardware needed.** Three native-radio boards A-B-C with espnowrelayblock on A and C for each other's MAC: send a TEXT flood and a routed espnowsend through B, power B off and watch the route expire. Separately, change espnowchannel on two boards and confirm key exchange survives.


## G-36 (P2, partly_confirmed): ESP-NOW file transfer radio path has no host run or hardware record

**Claim at 95dc3f34.** The RAM receive path is recorded on hardware: 2026-10-09 XIAO->QT Py imagesend of 7,986 B and 2,104 B, plus 1,024 B QT Py->XIAO, all byte-identical by SHA-256, and 2026-10-13 bond sync with a 70 KB manifest. The streaming path for files over 128 KB, receiver cancel, sender reboot with .part cleanup, and the slot/streaming code itself have no host harness and no dated record.

**Why this priority.** The bond and config distribution path (files under 128 KB, RAM receive) does have dated two-board records. What is missing is the >128 KB streaming, cancel and reboot cleanup used for larger user files, which is worth one bench run but does not block a trustworthy 1.0.

**Refuted or overstated.** 'No dated two-board record' is wrong for the RAM path: the 2026-10-09 test doc and the 2026-10-13 CHANGELOG verification scope both record successful, hash-checked ESP-NOW file transfers, including the bond manifest. The gap is limited to streaming over 128 KB, cancel and reboot cleanup.

**Evidence.**
- `docs/XIAO_QTPY_CAMERA_ESPNOW_TEST.md:77` refutation: imagesend (System_ImageManager.cpp 674 -> sendFileToMac) 7,986 B byte-identical; 108-114 normal-image 2,104 B and 1,024 B transfers matched; 96 says files above 128 KiB and loss/retry are not qualified
- `CHANGELOG.md:854` refutation: the 0.99.98 verification scope records 'bond sync with a 70 KB manifest' on XIAO/QT Py/P4X-EYE on 2026-10-13
- `components/hardwareone/System_ESPNow_Files.h:83` kFileSlotMaxFileSize 131072; 87-89 says bond/config files are always on the RAM path; the streaming path is used only above 128 KB
- `components/hardwareone/System_ESPNow_Files.h:28` FILE_CANCEL, 30 s slot timeout (96), boot .part-* cleanup (33, 104); none of it is host-run
- `components/hardwareone/test/host/README.md:4321` espnow_bond_json section: 'The radio transfer, the senders' sendFileToMac and the web response are not run.' The only other sendFileToMac reference in the tests is a stub in espnow_fs_transcript_privacy_harness.cpp:130
- `components/hardwareone/System_ESPNow.cpp:15545` sendFileToMac sender (unchanged apart from comments)

**Newer commits.** 64713c0d: System_ESPNow_Files.cpp/.h changed only in comments (cmd_exec_task -> cmd_exec); none of the six BLE commits is relevant

**Waveshare.** Does not apply: no ESP-NOW in the Waveshare saved profile.

**Bounded fix.** Run one two-board check: a 300 KB file (streaming path), a receiver-side cancel mid-transfer, and a sender reboot mid-transfer followed by the receiver's boot cleanup. Optionally add a slot/streaming harness over System_ESPNow_Files.cpp using the espnow_tx_multiframe doubles.

**Done when.** A dated record with sizes, throughput and SHA-256 equality for the 300 KB file; the receiver's file listing shows no .part-* after the reboot case; the cancel produces FILE_CANCEL and frees the slot within 30 s.

**Hardware needed.** Two native-radio boards (for example XIAO-S3 and QT Py or FeatherS3) with LittleFS space for a 300 KB file.


## G-37 (P2, partly_confirmed): C6 companion outage handling has no induced-outage hardware record

**Claim at 95dc3f34.** Soft recovery has dated P4X-EYE records: 2026-10-03 c6restart, c6hold on/off, and automatic soft recovery of a crashing companion; 2026-10-30 natural C6 resets that completed soft recovery; 2026-11-05 four controlled c6restart cycles from the four-link state, each recovering the phone, both arms and the ring in 13-49 s. Still host-only: the three-strike P4 reboot, the RTC marker and second-reboot refusal, and boot with the C6 held. Recovery with Wi-Fi associated is not stated in any record.

**Why this priority.** Soft recovery, the common outage path, is now recorded on hardware several times. Only the rare hard path (three strikes leading to a P4 reboot) and the no-radio boot are still host-only, and the reboot already finalizes any recording first.

**Refuted or overstated.** 'Never observed end to end' and 'CHANGELOG has no induced-outage bench line' are wrong. Soft recovery, c6hold and c6restart were verified on 2026-10-03 (CHANGELOG 0.99.96.1), and repeated c6restart recoveries with the glasses, ring and phone up are recorded for 2026-11-05. The recording-interruption concern is softened because rebootDevice finalizes a recording before restarting.

**Evidence.**
- `CHANGELOG.md:1260` refutation: 'Verified on hardware (P4X-EYE, 2026-10-03)': c6restart, c6hold on/off, the console mirror and automatic soft recovery of a crashing companion
- `CHANGELOG.md:1271` 'The deep-sleep hold and the once-per-power-cycle reboot guard are covered by host tests only'
- `docs/investigations/2026-11-04-p4x-fix-progress.md:427` a controlled C6 restart recovered the phone in 13.29 s, both arms in 43.8 s and the ring in 49.3 s; four restart cycles in total recovered all four links (acceptance.md:63)
- `docs/investigations/2026-11-04-second-p4x-bluetooth.md:197` October 30 records include C6 unexpected resets with completed soft recoveries
- `components/hardwareone/Radio_CompanionCore.h:31` CompanionPolicy unchanged: 3 missed beats, backoff 5 s..900 s, 3 soft recoveries before hard, hardRebootAllowed
- `components/hardwareone/System_RadioCompanion.cpp:740` HardReboot: SYSEVT_COMPANION_REBOOT, RTC marker, rebootDevice('c6-companion') which finalizes an in-progress recording (747); a second reboot is refused at 1110-1116
- `components/hardwareone/System_RadioCompanion.cpp:1128` boot without an answer posts companion_offline boot_failed and keeps retrying; main/hardwareone-idf.cpp:115 continues the boot when radioHalPrepare fails
- `components/hardwareone/test/host/CMakeLists.txt:412` radio_companion_core, esp_now_hosted_protocol (411), companion_ble_shutdown (1813), companion_reboot_contract (2090), radio_legacy_update (2179, new with Waveshare)
- ... 1 more in verify/E1.json

**Newer commits.** 64713c0d: System_RadioCompanion.cpp switches c6update to radioHalStartUpdateTransport and renames c6console to c6_console; ab387b02 / 3e5d4579: legacy companion compatibility limited to explicit c6update (Waveshare factory C6); 582f2c98: dated controlled c6restart four-link recoveries and the clean overnight companion stats (0 missed heartbeats)

**Waveshare.** Adds scope. The Waveshare family uses the same companion monitor over different SDIO wiring and can start from factory C6 firmware. Its docs record a C6-recovery panic on bring-up firmware that is not shown fixed, so Waveshare needs its own induced-outage check before it is listed as supported.

**Bounded fix.** Run one bench session per P4 board family covering the hard path and the no-radio boot, then record it.

**Done when.** A dated CHANGELOG/doc entry containing: c6status json before and after holding the C6 long enough for 3 soft recoveries, exactly one P4 reboot with the RTC marker log line and slow retries afterwards (no second reboot), a boot with the C6 held showing companion_offline boot_failed and a later recovery, and one soft recovery with Wi-Fi associated. On Waveshare, a sustained display plus radio run with an induced C6 reset and no panic.

**Hardware needed.** P4X-EYE and Waveshare P4 Touch LCD 5: c6hold long enough to trip three soft recoveries, a boot with c6hold set (or the C6 held in reset), and a c6restart with Wi-Fi associated and glasses, ring and phone up.


## G-40 (P2, confirmed): Login salt is the device key: a flash-UID read failure or representation change locks out every account

**Claim at 95dc3f34.** The PBKDF2 salt is getDeviceEncryptionKey() (mac+flashUID). selectDeviceKeyEpoch validates candidates only against AES blobs in settings.json. With no blobs, or with an unparseable settings.json (then the lazy getDeviceEncryptionKey runs), candidate 0 is used without validation. isValidUser hashes with that single key, with no retry across epochs.

**Why this priority.** The mechanism is real and its result is total lockout, but nothing in the tree shows the UID read failing or changing representation on a supported chip. It is a low-likelihood risk to narrow, not a 1.0 blocker.

**Refuted or overstated.** The proposed fingerprint fallback cannot recover a boot where the UID read itself failed, because the true UID is unknown, so the mac+flashUID candidate cannot be built. It can only detect that case and refuse or retry. It does help the representation-change (bswap) case. A flash chip that consistently lacks unique-ID support yields a stable mac+0 key and is safe. I found no hardware record of either failure mode.

**Evidence.**
- `components/hardwareone/System_User.cpp:2020` hashUserPassword salt = getDeviceEncryptionKey(); isValidUser (2772-2825) hashes once and compares
- `components/hardwareone/System_Settings.cpp:1797` selectDeviceKeyEpoch: uidOk=false -> only the mac+0 candidate; with nBlobs==0 best stays 0 (1823-1840) and only 'no stored secrets to validate against' is logged
- `components/hardwareone/System_Settings.cpp:766` lazy getDeviceEncryptionKey: one esp_flash_read_unique_chip_id call, no retry, falls back to UID 0
- `components/hardwareone/System_Settings.cpp:1776` header comment acknowledges that logins break because the PBKDF2 salt is this key
- `components/hardwareone/System_Settings.cpp:755` deviceKeyFingerprint exists but is only logged, never persisted or checked

**Waveshare.** New flash chip (32 MB on Waveshare P4). Whether esp_flash_read_unique_chip_id is supported there and stable across boots is not recorded. Only a consistent result matters.

**Bounded fix.** Retry the UID read a few times before falling back to 0. Store deviceKeyFingerprint in NVS or users.json at setup. At boot, try the candidates against it (with or without blobs), and on a mismatch log a durable [CRYPTO] event and keep retrying instead of silently adopting mac+0. Longer term, use a per-account random salt stored in the credential file.

**Done when.** Host runner: credentials written under the mac+UID key; boot with the UID read forced to fail once, then succeed: login works and the fallback event is logged. Boot with bswap representation: the stored fingerprint selects the right epoch.

**Hardware needed.** Boot logs from a FeatherS3, an ESP32 board and the P4/Waveshare showing 'device key: mac+flashUID' with the same fingerprint across 10 cold boots.


## G-46 (P2, partly_confirmed): P4X-EYE composite internal-heap low-water unmeasured after the October tranches

**Claim at 95dc3f34.** No P4X composite run (links + speech + web + ESP-NOW) has recorded internal minFree or the largest-block minimum since the October tranches. New snapshots exist: 223 KB free with a 147,456 B largest block (second P4X, 2026-11-04), and 215,675 B free after 8 h 44 min idle with glasses+ring+phone (2026-11-06). ESP-Hosted task margins under load are still unmeasured. One PSRAM heap corruption on 2026-11-06 remains unattributed.

**Why this priority.** The steady-state composite now looks healthy (215-223 KB free, 147 KB largest) and speech admission refuses rather than crashes; the remaining gap is a low-water under speech plus links, best folded into the G-42 P4 soak rather than gating 1.0 alone.

**Refuted or overstated.** The 31,812 B lifetime-minimum and the 293,480 B four-link figures cite docs/P4_MEMORY_AUDIT_2026-10-16.md and docs/MEMORY_PRESSURE_AUDIT_2026_10_21.md. Neither file is in the tree at 95dc3f34 or aa6508fd, so those numbers cannot be re-verified. 'Within one allocation of refusing speech' is overstated for the current state: post-tranche snapshots show 215-223 KB free.

**Evidence.**
- `docs/P4_RUNTIME_MEMORY.md:116` Boot-wide low-water 58,616 B (116) and 50,648 B (140) during speech; 82,980 B after the PSRAM pass (399)
- `docs/investigations/2026-11-04-second-p4x-bluetooth.md:170` Snapshot ~223 KB internal free, largest 147,456 B, zero instrumented allocation failures; snapshot only, not low-water
- `docs/investigations/2026-11-04-p4x-fix-progress.md:1111` Overnight: internal free 215,675 B (222,491 B at boot); no minFree recorded; no speech/web/ESP-NOW load
- `docs/investigations/2026-11-04-p4x-fix-progress.md:855` CORRUPT HEAP in a PSRAM block (tiT, lwip_cyclic_timer) on a console-started note, writer unidentified; it did not reproduce under heap poisoning (923-952)
- `CHANGELOG.md:263` ESP-Hosted task stacks (BLE-12) 'overflow margins unmeasured under load' still stands
- `components/hardwareone/stt/quartznet_runtime.cpp:156` 150 KiB cold-construction admission 'needs hardware peak qualification'

**Newer commits.** d0faa77b: heap-poisoning diagnostic profile (p4-phone-pipe-heapdiag) available for the composite run; 582f2c98/95dc3f34: records the new P4X snapshots and the unattributed PSRAM corruption

**Waveshare.** Waveshare adds a DSI framebuffer and touch on the same 32 MB PSRAM / internal SRAM budget, with glasses, phone and local STT enabled. It has no composite low-water record either.

**Bounded fix.** Fold into the G-42 P4 soak: the composite workload (links + 3 speech sessions + web session + one ESP-NOW peer) with memreport json every 30 s and taskstats json every 60 s, recording dram.minFree, the internalLargest minimum and the five hosted-task minima.

**Done when.** Tracked record of a >= 2 h composite run: dram.minFree >= 40 KB, internalLargest never < 16 KB, no hosted task < 1 KB min free, zero allocation failures and no unexplained link drops.

**Hardware needed.** P4X-EYE with glasses, ring, phone, a browser session and one ESP-NOW peer for >= 2 h.


## G-49 (P2, partly_confirmed): Explicit light sleep is one OLED press away, ignores the API return value and has no hardware record

**Claim at 95dc3f34.** cmd_lightsleep ignores the esp_light_sleep_start() return and always reports 'Woke from light sleep'. The OLED Power > Sleep submenu runs 'lightsleep 20' on press with no confirm, while Restart and RAM flush have one. No record shows whether glasses, ring, phone or Wi-Fi links survive or come back after a light sleep on any board.

**Why this priority.** Explicit light sleep is an admin, opt-in action whose dropped links are expected. Checking the return value and adding an OLED confirm are trivial polish, not trust blockers.

**Refuted or overstated.** 'One OLED press away' is overstated: light sleep sits two levels down (Power -> Sleep -> row 1), and guests are blocked (oledGuestBlocksMutate). The web path is already behind a confirm. A light-sleep hardware record does exist: a 2026-10-08 QT Py OLED run where the device woke. Only the link-survival part has no record.

**Evidence.**
- `components/hardwareone/System_Utils.cpp:1564` esp_light_sleep_start() result discarded; :1577 always returns 'Woke from light sleep'. OTA-probation refusal (:1506) and anti-flap cooldown (:1515) guards exist
- `components/hardwareone/OLED_Mode_Power.cpp:224` Sleep submenu row 1 -> executeOLEDCommand("lightsleep 20") with no confirm; Restart and RAM flush use oledConfirmRequest (:177, :180)
- `components/hardwareone/WebPage_Power.cpp:368` the web light-sleep button already asks hwConfirm, warning that the web interface stops responding
- `components/hardwareone/System_RadioCompanion.cpp:1204` P4 hooks only pause and resume the companion watchdog
- `components/hardwareone/System_WiFi.cpp:1959` WIFI_PS_NONE forced; CONFIG_PM_ENABLE unset (config/sdkconfig:1546)
- `docs/QTPY_OLED_TEST.md:95` dated 2026-10-08 hardware record: QT Py woke from 2 s light sleep with the OLED active and stopped. No radio links were tested
- `docs/CROSS_BOARD_POWER_POLICY.md:43` now tracked; notes the ignored return value and calls for an audit

**Newer commits.** 5e28cd3a / 138c59fb: Waveshare display-idle and DSI downclock protection; not light-sleep paths, and the effect of light sleep on Waveshare DSI scanout has no record

**Waveshare.** The same shared Power menu is reachable by touch on Waveshare. Light sleep there also pauses DSI refresh and the ESP-Hosted SDIO link. There is no record of the panel, backlight or C6 link recovering afterwards.

**Bounded fix.** Check the esp_light_sleep_start() return and report 'Error: light sleep rejected: <esp_err>'. Put the OLED Sleep row behind oledConfirmRequest like Restart.

**Done when.** A host harness scenario returns the error text on a failed sleep call, and the OLED row shows a confirm. A dated note records the return code and per-link survive or reconnect times after `lightsleep 20` on a FeatherS3 and a P4X-EYE or Waveshare.

**Hardware needed.** `lightsleep 20` with glasses, ring and Wi-Fi connected on a FeatherS3 and a P4X-EYE (or Waveshare), recording per-link reconnect within 60 s and display recovery.


## G-53 (P2, partly_confirmed): Wi-Fi driver persists the PSK in plaintext NVS on every default board

**Claim at 95dc3f34.** On native-Wi-Fi boards (feathers3, xiao_s3, qtpy_esp32, feather_esp32_v2) the firmware brings Wi-Fi up through Arduino WiFi.mode(). Arduino's _persistent defaults to true, and nothing calls WiFi.persistent(false) or esp_wifi_set_storage(WIFI_STORAGE_RAM). With CONFIG_ESP_WIFI_NVS_ENABLED=y, every esp_wifi_set_config(WIFI_IF_STA) persists the PSK in the plaintext nvs partition. On the P4 boards (p4x_eye, waveshare_p4_touch_lcd_5), Wi-Fi is ESP-Hosted: Arduino forces nvs_enable=false and RAM storage on the host, and the C6 slave adopts the host's nvs_enable.

**Why this priority.** Real on S3/ESP32 boards, but it adds no new exposure class: with flash encryption off, settings.json's AES key is derived from the eFuse MAC and flash UID (readable with esptool), so physical access already yields the PSK. It is a one-line hygiene fix plus a documentation point, not a release blocker.

**Refuted or overstated.** 'Every default board' is wrong: the P4X-EYE and Waveshare P4 hosts run Wi-Fi with nvs_enable=false and RAM storage, and the C6 slave follows the host setting (on hardware this still needs an NVS dump of the C6 to confirm). 'The settings.json AES layer is bypassed' overstates the protection that layer gives: without flash encryption its key comes from device identifiers, so it is device binding, not secrecy against physical access.

**Evidence.**
- `components/hardwareone/System_WiFi.cpp:1849` esp_wifi_set_config(WIFI_IF_STA, &wifi_config); again at 2066.
- `components/hardwareone/System_WiFi.cpp:734` Bring-up via WiFi.mode(...), which calls wifiLowLevelInit(_persistent).
- `components/arduino/libraries/WiFi/src/WiFiGeneric.cpp:393` _persistent = true by default. Line 292: RAM storage only if !persistent. Lines 265-269: under CONFIG_ESP_WIFI_REMOTE_ENABLED, cfg.nvs_enable=false and persistent=false (vendored, git-ignored core).
- `config/sdkconfig:1731` CONFIG_ESP_WIFI_NVS_ENABLED=y.
- `boards/feathers3.defaults:37` Flash and NVS encryption off (37, 46). Same in xiao_s3 (31, 40), p4x_eye (24, 25) and waveshare_p4_touch_lcd_5 (13, 14). Only feathers3_fe encrypts (26, 31).
- `.sdk-overrides/esp32p4/espressif__esp_hosted/slave/main/slave_wifi_std.c:455` The C6 slave uses the host's cfg.nvs_enable (local, git-ignored SDK override). WIFI_STORAGE_FLASH is set only in the network-split/pre-provision example path (esp_hosted_coprocessor.c:1113).
- `components/hardwareone/System_Settings.cpp:766` getDeviceEncryptionKey derives the settings AES key from ESP.getEfuseMac() and the flash unique ID, both readable by anyone holding the board.

**Waveshare.** Waveshare P4 is unaffected on the host side (ESP-Hosted, nvs_enable=false), like the P4X-EYE.

**Bounded fix.** Call WiFi.persistent(false) (or esp_wifi_set_storage(WIFI_STORAGE_RAM)) once before the first WiFi.mode() in the bring-up path, since credentials are restored from settings.json. Say in SECURITY.md that physical access discloses stored credentials unless flash encryption is enabled.

**Done when.** `grep -n 'persistent(false)\|esp_wifi_set_storage' components/hardwareone/System_WiFi.cpp` shows the call. On an S3 after connecting, `nvs_partition_parser` on a read_flash dump of the nvs partition shows no nvs.net80211 sta.pswd entry.

**Hardware needed.** On a XIAO S3 or FeatherS3: connect, esptool read_flash the nvs partition and parse it for nvs.net80211/sta.pswd, before and after the fix. Optionally the same on the P4X-EYE's C6 to confirm the slave leaves nothing.


## G-54 (P2, partly_confirmed): No BLE link-layer security; G2/R1 peers trusted by saved address or advert-name pattern

**Claim at 95dc3f34.** No production code configures SMP pairing, bonding or link encryption. The phone pipe actively refuses unsolicited SMP and relies on its app-layer pairing code. G2/R1 peers are trusted by advertised address once saved (any advert name only when no address is saved), so a device that clones the static address could impersonate them. The BLE console accepts plaintext logins until a blesecret is set, because bleScRequired() needs a non-empty secret. The BLE server is off by default and a boot notice warns that BLE is unencrypted. Not demonstrated on hardware.

**Why this priority.** No BLE link-layer security is a known, documentable design limit with partial mitigations already in place (saved-address filter, opt-in BLE server, boot warning, app-layer secure channel); for 1.0 it needs a SECURITY.md statement rather than an SMP project.

**Refuted or overstated.** Proposed fix (1) is already the behaviour: a saved MAC is an authoritative filter, and name-match is used only when no MAC is saved (G2_Glasses.cpp:3625-3630, 3693-3707, 13764-13768). Part of fix (2) exists: the boot notice and the status JSON already say plaintext. The residual risk is address spoofing plus plaintext console credentials before provisioning.

**Evidence.**
- `components/hardwareone (grep esp_ble_gap_set_security_param|esp_ble_set_encryption|NimBLESecurity|ble_sm_):None` Zero hits in production sources; SMP work exists only as experiments/p4_ble_roles/diagnostics/ (diagnostic builds).
- `components/hardwareone/BLE_PhonePipe.cpp:326` ESP_GAP_BLE_SEC_REQ_EVT answered with esp_ble_gap_security_rsp(..., false).
- `components/hardwareone/G2_Glasses.cpp:3693` With a saved-MAC filter, only an exact address match is classified as a temple; the name is ignored.
- `components/hardwareone/G2_Glasses.cpp:13768` Repair scan uses name-based match only when the side has no saved MAC.
- `components/hardwareone/System_BleSecureChannel.cpp:164` bleScRequired() = flag && secret non-empty.
- `components/hardwareone/Bluetooth.cpp:3974` bleSecurityBootNotice prints 'Bluetooth is UNENCRYPTED' when the flag is on and the secret is empty.
- `components/hardwareone/Bluetooth.cpp:4307` BLE info JSON reports secureChannelRequired = bleScRequired().
- `components/hardwareone/Bluetooth.cpp:4617` bluetoothAutoStart default false; bluetoothRequireAuth default true (4618); bleRequireSecureChannel default true (4622).

**Newer commits.** c1d1dbac / 295a6a7f: Waveshare enables the glasses and phone links; no change to the link-security model

**Waveshare.** Waveshare now enables the G2 and phone links (c1d1dbac), so the same address-trust model applies to it.

**Bounded fix.** Document in SECURITY.md that BLE peers are address-trusted and the console is plaintext until `blesecret` is set. Optionally refuse BLE console login while bleRequireSecureChannel is on and the secret is empty. Defer SMP bonding.

**Done when.** SECURITY.md states the BLE model. If the refusal is added, ble_sc_policy_harness gains the case 'flag on, secret empty => login refused'.

**Hardware needed.** To settle impersonation: a second ESP32 set to a saved G2 temple's address that advertises the G2 name, with the real temple off. Observe whether the firmware connects and acts on its notifications.


## G-55 (P2, partly_confirmed): Authorization gates lack executable tests for reserved-name sentinel, bonded FS-RPC and metadata scrub

**Claim at 95dc3f34.** No host case puts a reserved name (AuthBypass, system) into the roster and asks isSuperAdminUser/isAdminUser. roleFromRosterJson and the authority cache have no reserved-name filter, so an on-roster 'AuthBypass' superadmin would make the auth-off sentinel super-admin. The FS-RPC bonded-only gate and the metadata byte policy are already tested.

**Why this priority.** Two of the three claimed gaps already have host coverage; the remaining one is a defence-in-depth case for a roster state the firmware cannot itself create once G-52 is fixed.

**Refuted or overstated.** 'FS-RPC bonded-only gate has no dedicated harness' is refuted: test_espnow_fs_transcript_privacy.py checks who is served and who is refused. 'metadata scrub has no harness' is refuted: test_espnow_metadata_text.py and espnow_metadata_text_harness.cpp pin the byte policy. Both were present at v0.99.99.

**Evidence.**
- `components/hardwareone/System_User.cpp:1089` isSuperAdminUser: bond-name special case, then accountAuthority, then superFromRosterJson. No isReservedUsername check.
- `components/hardwareone/HardwareOne.cpp:620` hasSuperAdminPrivilege just calls isSuperAdminUser(ctx.user) for non-bond users, including 'AuthBypass'.
- `components/hardwareone/test/host/user_roster_authority_harness.cpp:None` No AuthBypass/system roster fixture (grep empty).
- `components/hardwareone/test/host/espnow_fs_transcript_privacy_harness.cpp:249` Already checks refusals for 'a non-bond peer', 'the bonded peer without a live session', 'no bond configured' and bond mode compiled out (present at v0.99.99).
- `components/hardwareone/test/host/espnow_metadata_text_harness.cpp:326` Already checks metadataTextByteAllowed byte-by-byte (added c8eb7d9b, in v0.99.99); registered at test/host/CMakeLists.txt:477.
- `components/hardwareone/test/host/remote_command_authority_harness.cpp:371` AuthBypass is covered as a non-roster identity being refused admin, but not as an on-roster account.

**Bounded fix.** Filter reserved names in authority_cache::fill/accountAuthority (or in isAdminUser/isSuperAdminUser). Add one user_roster_authority case: roster {AuthBypass: superadmin} gives isSuperAdminUser("AuthBypass") == false and isAdminUser("AuthBypass") == false.

**Done when.** The new case exists in user_roster_authority_harness.cpp and passes in the host suite.


## G-57 (P2, confirmed): Four disagreeing roster parsers and first-user fallbacks decide privilege

**Claim at 95dc3f34.** System_User.cpp still has four roster parsers (roleFromRosterJson, adminFromRosterJson, superFromRosterJson, legacyRoleFromRosterJson; 20 references). They disagree on case, banned and missing role, and three carry first-user fallbacks: role-less first account becomes superadmin in roleFromRosterJson, admin in adminFromRosterJson, and super in superFromRosterJson whenever no account is explicitly superadmin, whatever the first account's own role. These fire only on hand-edited, restored or synced rosters. Nothing documents the rule or pins it with a role-less fixture.

**Why this priority.** The fallbacks are dormant on any roster this firmware creates (the founder is written as superadmin and protected from ban and demotion, and command admission fails closed on role lookup), so it is a maintainability and restore-path risk, not a live 1.0 defect.

**Refuted or overstated.** The cited CHANGELOG evidence is mis-described: CHANGELOG.md:2185 records that an empty identity matched the first-user fallback when users.json could not be parsed, not that 'demotion of the first account did not take effect'. The privilege-escalation reading is overstated, because the fail-closed role check in authorizeCommand runs before the admin and super gates.

**Evidence.**
- `components/hardwareone/System_User.cpp:739` Cache comment: the four verdicts 'deliberately disagree'.
- `components/hardwareone/System_User.cpp:1017` superFromRosterJson fallback: !anyExplicitSuper && who == firstUser.
- `components/hardwareone/System_User.cpp:919` adminFromRosterJson fallback: first user with no role field.
- `components/hardwareone/System_User.cpp:1075` roleFromRosterJson: missing role on the first account becomes 'superadmin'.
- `components/hardwareone/System_User.cpp:1021` legacyRoleFromRosterJson ignores banned and defaults to 'user'; used for the guest branch.
- `components/hardwareone/System_Utils.cpp:4111` authorizeCommand first requires getUserAuthorizationRole (honours banned, fail-closed), so a banned account is denied even though superFromRosterJson ignores banned.
- `components/hardwareone/System_FirstTimeSetup.cpp:297` The founder is always written with role superadmin. Founder (id 1) is protected from ban, demote and delete (System_User.cpp:2662, 3176).
- `components/hardwareone/WebServer_MigrationTool.cpp:None` No role normalisation of a restored roster (grep role: only a guest web gate include).

**Bounded fix.** Pick one option and record it. (a) Remove the three first-user fallbacks and normalise or refuse role-less rosters at restore and sync. (b) Keep them, document the recovery rule in USERGUIDE.md, and add a host fixture (role-less roster, banned guest) that pins the current verdicts.

**Done when.** user_roster_authority (or a sibling suite) has role-less-roster and banned-guest fixtures encoding the chosen rule and passes; USERGUIDE.md states the rule if (b) is chosen.

