# Hardware One 1.0 release-readiness assessment

Date: 2026-11-06. Revision verified: `95dc3f34` on `codex/main-sync` in
`~/Documents/Codex/Projects/hardwareone` (clean tree; 47 commits after v0.99.99 `aa6508fd`,
4 commits after `origin/main` `4ed1eaa5`). The other Mac's 84 uncommitted files (OLED CLI
rewrite, OLED text bounds, NetworkEvents fix) were not assessed.

Files: the master copy, Appendix A, raw verdicts (`verify/`) and the 2026-10-30 handoff
package are in `/Volumes/Audiodrive/stt/hw1-work/reviews/release-readiness-1.0/`. Copies of
this report and Appendix A sit untracked in the checkout's `docs/`.

This is an assessment and plan. Apart from those two untracked copies, nothing in the repository was modified, nothing was
flashed, nothing was published. Two passes produced it:

- 2026-10-30, other Mac: 16 read-only investigation lanes at v0.99.99 (141 findings,
  consolidated into 89 groups), plus host checks and one clean-checkout FeatherS3 build.
  The verification pass that was meant to follow did not run (spend limit).
- 2026-11-06, this Mac: one skeptic per group re-opened every cited path at `95dc3f34` and
  tried to refute it, for all 61 P0/P1 groups (8 Opus agents). The host suite and Python
  tool tests were re-run at `95dc3f34`. Per-group detail is in Appendix A
  (`appendix_A_verified_groups.md`); raw verdicts are in `verify/*.json`.

Evidence vocabulary used throughout: **implemented** (code exists), **host-tested**
(an executable host test covers it), **builds** (an image was produced), **hardware-qualified**
(a dated physical record exists), **fixed since** (an earlier defect is gone at 95dc3f34),
**confirmed defect**, **evidence gap**, **unverified risk**, **optional improvement**.
A passing build or host test is never counted as hardware qualification.

---

## 0. Act now, independent of 1.0

**The repository is public** (`gh repo view`: PUBLIC), and all 57 tags are on GitHub
(`git ls-remote --tags origin`). The root commit `a8014ca4`, still the only root of HEAD,
contains `components/hardwareone/nocaseseriallogsespnowtest.txt`, a serial capture with the
home Wi-Fi passphrase, SSID/BSSID, LAN IP and an admin username/password. It is reachable from
`origin/main`, 5 other GitHub branches and every tag through v0.99.99. In tag v0.99.92, 36 of
the 186 files under `docs/` are internal audit, security and review documents, including
`docs/AUTH_SECURITY_REVIEW.md`.

1. Rotate the Wi-Fi passphrase and that admin password today. That closes the real exposure
   whatever happens to the history.
2. Before tagging any release candidate (a later rewrite changes every tag hash), decide whether to rewrite public history (git-filter-repo of the capture file and the
   v0.99.92 review docs, re-create tags, force-push, ask GitHub support to purge cached views).
   Rewriting breaks every existing clone and fork, so it is the owner's call; after rotation
   it is hygiene, not containment.

This is G-04 (P0); the HEAD-side cleanup is in workstream P0-1 below.

---

## 1. Overall judgment

The feature work is done. What is left is mostly about whether outsiders can trust the
release, with a small number of real code defects. The project is closer to a credible 1.0
than the length of this list suggests, because most items are bounded documentation,
packaging and test-hygiene fixes. It is not one tag away, though: three things stand between
the current tree and an honest public 1.0.

1. **A stranger cannot build it.** A fresh clone fails every board: `components/arduino`
   is git-ignored and no tracked script vendors and patches it (G-01). The documented
   manual procedure worked at v0.99.99, but it now replays to the wrong tree because
   `docs/arduino-local-patches/README.md:54-62` omits the 2026-11-06 patch (A1 replay:
   15 markers, `verify_patches.sh` fails).
2. **The update/recovery story is unqualified.** The only dated OTA hardware record is
   FeatherS3, 2026-08-05, v0.99.8. The recovery updater changed on 2026-10-13 and has not run
   on hardware since; the deployment OTA layouts and QT Py OTA have no record at all (G-09).
   OTA must be qualified on the RC binaries or labelled experimental.
3. **Nobody has written the release down.** No supported matrix (G-14), no compatibility
   or upgrade promise (G-12), no licence notice (G-05), an empty `[Unreleased]` across 47
   commits (G-23), and a host suite that has been red on main for a week with nothing gating
   it (G-17, Section 3).

Most of the code is in good shape. Of the 61 groups verified, none was found to be a
widespread runtime fault. The confirmed code defects are local and bounded: two web XSS
paths (G-50, G-51), the reserved-owner-name lockout (G-52), log-cap and corrupt-file handling
(G-38, G-39), the task watchdog configuration (G-11) and the classic-board G2 admission floor
(G-44). There is also one probable regression from this morning's BLE commit (N-1, found by
reading code, not yet checked by a skeptic or on hardware). Hardware qualification is
the scarce resource. Plan around a small set of bench runs, not a large test programme.

Realistic size: about 2 weeks of focused fixes and docs, then a 1 to 2 week RC with the bench
runs listed in Section 6. The new Waveshare board should go into 1.0 as experimental rather
than delay it.

Verification results at a glance (61 groups): 39 confirmed, 21 partly confirmed (some part
overstated or refuted), 1 fixed since v0.99.99 (G-33, the G2 half-connected escalation).
Re-prioritised: 6 of the 12 P0 groups stay P0 (G-01, G-04, G-05, G-09, G-10, G-12) and 6 moved
to P1 (G-02, G-03, G-06, G-07, G-08, G-11); 18 P1 groups moved to P2. This report's own
workstreams then fold several P1 groups back into P0 because they are the same change, and
raise the host-suite gate to P0 (Section 4).

---

## 2. What exists and should not be rebuilt

Section 2 of the 2026-10-30 draft (`40_report_draft/report_part1_method_checks_keeplist.md`)
remains accurate at 95dc3f34, with these updates from verification:

- The previously uncommitted layer is committed (64713c0d): showcase tool, CPU sampler, task
  renames, G2 half-connected escalation (host-tested by ble_recycle_phone and
  g2_repair_schedule), power-policy docs, the Android prompt doc (labelled historical).
- BLE lifecycle hardening landed with host tests in each commit: saved-mate retention
  (fbc6fecc), disconnect reasons and semaphore release in the Arduino BLE client (d5f7b9ac,
  recorded in `tools/ble-phone-pipe/arduino.patch` with a re-pinned manifest), ring setup
  handles (a9d47ea3), phone pipe CLOSE/DISCONNECT isolation (37b2fa23), setup-worker client
  retirement and overrun watch (d0f050f9), richer crash records (d987fdf5).
- Dated hardware records added after v0.99.99: P4X C6 soft recovery (four `c6restart` cycles
  2026-11-05, all links back in 13 to 49 s), two long P4X runs (3.8 h and 8 h 44 min,
  `docs/investigations/2026-11-04-p4x-fix-progress.md`), ESP-NOW RAM-path file transfers
  and bond sync on two boards (2026-10-09, 2026-10-13), and Waveshare bring-up acceptance
  records (display, touch, backlight, camera, mic, STT model read).
- `tools/p4/ram_monitor.py` is a partial soak-capture tool; extend it rather than write a new one.
- The generated `COMMAND_REFERENCE.md` matches a fresh regeneration byte for byte (E2).
- The Python tool tests are green (114/114) and the OTA tool tests are green (53/53).

---

## 3. Host checks run at 95dc3f34 (2026-11-06)

| Check | Result |
|---|---|
| Native host suite (`components/hardwareone/test/host`), -j6 | 328 of 338 passed; 4 browser suites not run (no Node or Deno on this Mac); 10 failed |
| The 10 failures re-run serially | 9 fail; `stt_continuous_full_phrase` passes serially (parallel-only flake) |
| `python3 -m unittest discover -s tools -t .` | 114/114 OK (the three web-UI failures of 2026-10-30 are gone) |
| `tools/ota/tests` | 53/53 OK |
| Not run | Firmware builds, browser suites, phone crypto/TLS suites (need `HW1_PHONE_TEST_IDF`), updater host test, any device test |

The nine failures, none pointing to a firmware runtime defect by itself:

| Suite | Cause | Since |
|---|---|---|
| tof_poll_outcomes | harness lacks `announceUnresponsive`, `SYSEVT_SENSOR_RECOVERED` | red at v0.99.99 |
| tool_pins | secure-event catalog pins 165 kinds / 3149 B, fixture is 174 / 3307 | red at v0.99.99 |
| wifi_connect_budget | unused `SYSEVT_WIFI_CONNECTED` under -Werror; code moved to `wifiEventLogDrain` | committed in 64713c0d |
| oled_owner_requests | harness calls removed `prepareCameraPreviewData` | camera viewer work |
| optional_peripheral_config | asserts Waveshare has camera and mic off; 77fcbdcf/131f4c0e turned them on | Waveshare |
| stt_conv_scratch | `extract_block` cannot find its source anchor | source moved |
| ble_console_phone_link | extracted `phone_link.cpp` no longer compiles in the harness | needs a look |
| ble_central_coordinator | `-Wuninitialized-const-pointer` (new in Apple clang 21) | toolchain |
| event_catalog_structure | walks the whole working directory and reads an untracked non-UTF-8 header under `work/` | environment |

**Update, later on 2026-11-06 (uncommitted working tree on 95dc3f34):** eight of the nine are
fixed (harness re-extraction, re-pinned catalog counts, updated Waveshare expectations, clang-21
initializers, a git-file-list walk) and the two `stt_continuous` suites run serially, so ctest
gives 337/338 both with -j6 and -j1. The remaining `stt_conv_scratch` failure is real
environment drift: this Mac's staged `.sdk-overrides/esp32p4/espressif__esp-dl` fails
`prepare_esp_dl.py --check` ("provenance mismatch") and lacks code the tracked
`tools/p4/esp-dl-3.3.12-hw1.patch` has carried since v0.99.98.5. CMake only checks that the
override exists (`CMakeLists.txt:751-757`), so P4 images built here may use stale ESP-DL code.
It had been staged from the patch as of a27c59e4 (2026-10-11), so P4 images built on this
Mac since then lacked the 2026-10-25 (v0.99.98.5) ESP-DL prepared-args changes. Re-staged
with `tools/p4/prepare_esp_dl.py` from the cached pristine 3.3.12 package; `--check` now
verifies and `stt_conv_scratch` passes, so all 338 registered suites pass on this Mac
(browser suites still not run). Making the build run the `--check` belongs in P1-13. The Arduino patch README now lists the 2026-11-06 patch; an upstream 3.3.5
clone plus the documented sequence reproduces the vendored tree file for file.

`oled_mode_availability` and `oled_layout`, red on 2026-10-30, now pass. The real defect is
that nothing gates the suite: it has stayed red on main through 47 commits.

---

## 4. Backlog

Items are grouped into workstreams that share one change or one bench session. Group ids
point into Appendix A, which has the evidence (file:line at 95dc3f34), bounded fix,
definition of done and hardware need for each.

Confidence labels: **[defect]** confirmed defect, **[gap]** evidence gap, **[risk]**
unverified risk, **[retest]** stale finding needing retest, **[optional]** optional improvement.

### P0: must complete before 1.0

**P0-1. Public tree hygiene and secrets.** G-04 [defect].
After the rotation in Section 0: replace the account home path (5 docs, 2 of them new
Waveshare docs), home LAN subnet (4 files) and home SSID (3 docs) at HEAD with placeholders,
and decide on the history rewrite. Device MACs at HEAD are already placeholders (4ed1eaa5),
so that part is refuted.
*Done when:* `git grep -nIE '/Users/<account>|<home subnet>|<home SSID>' -- . ':!components/hardwareone_libs'`
is empty (the exclusion skips upstream author paths in vendored Doxyfiles), the passphrase and password are rotated, and the history decision is written in RELEASING.md.

**P0-2. Licence and notices.** G-05 [defect].
LICENSE.md is PolyForm-Noncommercial while `docs/USERGUIDE.md:1907` says MIT. There is no
NOTICE file, the A Dark Room MPL-2.0 text sits only in an ignored directory, and
arduino-esp32 (LGPL) and the NeoPixel library (LGPL) are statically linked under a
noncommercial licence. Fix the USERGUIDE line, add a tracked NOTICE.md (hardwareone_libs
SRC_DIRS, managed components, arduino-esp32, es8311, A Dark Room) and track the MPL text.
The owner must decide the LGPL question: keep and document how users can relink, or swap
NeoPixel for IDF `led_strip`.
*Done when:* NOTICE.md is tracked and complete, USERGUIDE has no MIT line, and the LGPL decision is written down.

**P0-3. A clean clone builds.** G-01 [defect], with G-02, G-24, G-25 (P1) folded in because they are the same change.
- Add the missing 2026-11-06 patch line to `docs/arduino-local-patches/README.md:54-62`
  and the `verify_patches.sh` hint. This is the one-line cause behind today's replay failure.
- Add a tracked `tools/vendor_arduino.sh`: clone arduino-esp32 at `11bc7ac1a`, apply the
  snapshot, dated patches and `tools/ble-phone-pipe/arduino.patch` in order, then run
  `verify_patches.sh` and `check_sdk.py`.
- Make it step 1 in README and QUICKSTART.
- Give `tools/build_deployment.sh` the same Arduino preflight, patch guard and a dirty-tree refusal (G-24).
- Note: `components/arduino` in the 95dc3f34 checkout has no `.git` of its own, so the
  README's `git -C components/arduino diff` refresh and the NetworkEvents check in
  `verify_patches.sh:113-120` cannot work there. The script should create the nested repo.
*Done when:* in an empty directory with only ESP-IDF 5.5.5, a clone of the RC commit plus the
vendor script plus `tools/build_board.sh feathers3` exits 0 with BUILD_INFO "clean tree", and
the same holds for every board in the release matrix.

**P0-4. Green, gated host suite.** G-17, G-19 [defect]; raised from P1 because tagging 1.0
on a red suite is not credible and the fix is bounded.
Fix the nine harnesses in Section 3 (mostly re-pinning or re-extracting), make
`event_catalog_structure` walk `git ls-files`, make the browser suites run (document Node, or
track the Deno shim as a test helper), list the vendored Arduino copy as a host-suite
prerequisite, and register or remove the stand-alone `hw1_ota_protocol` suite. Add "host
suite green" as a RELEASING.md step. A minimal CI job (G-16) is the durable form of this
gate, but a scripted local gate is enough for 1.0.
*Done when:* ctest at the RC commit reports 0 failures and 0 "Not Run" outside documented
optional suites, run serially and in parallel, and RELEASING.md requires it.

**P0-5. Define the release: matrix, scope and fit.** G-14 [gap], raised from P1 because the
matrix is what every other P0 is measured against; G-13, G-06, G-61 (P1) folded in.
- Publish the supported matrix in README (Section 5): tier, update path, and a link to the last dated hardware record.
- Build each stable row once from the clean clone at the RC commit and record headroom. The
  last S3 figures are 0.1% (bare `xiao_s3`) and 1.2% (`feathers3`), both from before v0.99.99,
  and the S3 BLE code has grown since.
- Build README's `tools/build_board.sh xiao_s3` example once (last measured 3,456 B free,
  before v0.99.99); if it no longer fits, point the example at `xiao-default` (G-61). Docs claim
  `boards/xiao_s3.defaults` turns Bluetooth off; it does not, and `xiao-default.json` and
  `xiao-no-bluetooth.json` are identical.
- Decide automations for the P4 handhelds. The handheld contract compiles them out against
  README:94; amend the docs or the contract, and build the contract once (G-13).
- Add a headroom floor (for example 64 KB) to `gen_build_info.py` so an image that no longer
  fits fails loudly.
*Done when:* the matrix table is tracked, every stable row has a BUILD_INFO from the RC
commit with headroom at or above the floor, and README examples build.

**P0-6. Install, update and recovery are true for the matrix.** G-09 [gap], G-10 [defect], with G-08 and G-29 (P1) folded in.
- OTA (G-09): on the RC's own updater and main pair, on each OTA row of the matrix (FeatherS3,
  Feather V2, XIAO Pocket Assistant; or move a row to cable-only), run once: migration flash, recovery console, SoftAP upload, signed staged apply with
  probation and `otaack`, forced rollback to recovery, interrupted write with resume.
  Record dated PASS lines. Any layout without such a record is labelled experimental.
  RELEASING.md gains the signing-key custody and bundle-publication steps.
- First install (G-08): QUICKSTART must name `erase-flash` for a used board and print a
  remedy at "FATAL: Filesystem initialization failed". The board is recoverable and no data is
  lost, which is why this is no longer P0 on its own, but it is the first thing a returning user hits.
- OTA docs (G-29): fix the stale statements, including README:139 (true for bare `xiao_s3`
  but with no pointer to the pocket_assistant OTA layout), the wrong size comments in the
  `partitions_ota_no_sr_16mb.csv` header and the uncaptioned current table in
  OTA_DEVICE_SETUP, the `otastatus` text that omits qtpy_esp32, and `ota status` vs `otastatus`. Add an "Update and recovery" section to USERGUIDE.
- Account recovery (G-10): USERGUIDE presents `factoryreset` as recovery for lost admin
  credentials, but it needs a super-admin login, so a single-owner device can only recover by
  cable erase. Document the true procedure (second admin uses `userresetpassword`; otherwise
  erase the LittleFS region, which loses settings and captures) and test it once. The same
  paragraph wrongly says only accounts are wiped. A physical re-arm is P2.
*Done when:* the 1.0 CHANGELOG carries the OTA PASS lines (or the experimental labels), and
QUICKSTART and USERGUIDE describe install, update, recovery and account recovery as tested on the RC.

**P0-7. Compatibility and upgrade promise.** G-12 [gap], with G-21 and G-23 (P1) folded in.
- Add a tracked `docs/COMPATIBILITY.md`: C6 companion firmware 2.12.13 exact (stock and
  Waveshare factory C6 need `c6update`), minimum phone-link v2 app build, CM5 protocol 1, G2
  profile versions, R1 identification strings, Raspberry Pi service version.
- State what 1.x promises. Suggested: 1.0.0 is a full-erase install; within 1.x, settings,
  users and automations are kept; renamed keys or events get an alias or a CHANGELOG
  "Changed" line; OTA layout ids and manifest format are frozen.
- Fix the rename that already breaks that promise: saved automations using `g2_worn` silently
  never fire after 64713c0d renamed it to `g2_plugin_alive`. Add an alias in
  `systemEventKindFromName`.
- Restore the user command `c6console`: the task rename in 64713c0d changed the command name
  too (G-21), or keep the new name and alias the old one.
- Fill CHANGELOG `[Unreleased]` for the 47 commits (Waveshare, event rename, powerdim removal,
  BLE fixes) and move the version from its seven hand-maintained sites to one (G-23).
*Done when:* COMPATIBILITY.md is tracked and linked from README and RELEASING.md, `g2_worn`
resolves, and `[Unreleased]` lists every user-visible change since v0.99.99.

### P1: worthwhile before 1.0

The P1 items are bounded, but there are many. Do the "before RC" set; the rest can slip to
1.0.x if the RC is otherwise clean.

**Before RC (cheap code fixes):**

| Item | Groups | Problem | Bounded fix |
|---|---|---|---|
| P1-1 | N-1 [risk, code-read only; new] | d0f050f9's overrun watch raises the BLE lifecycle fault at 240 s (`BLE_SetupWorkerCore.h:179`). `ring scan` allows up to 300 s (`G2_Ring.cpp:7410`) and ends early only on a stop request, ring yield, a generation change or another queued job (`G2_Ring.cpp:5642-5646`). The fault is cleared only by `closeble` or a stack recycle (`BLE_CentralStack.cpp:304`, `G2_Glasses.cpp:19393`, `Bluetooth.cpp:2381`), not when the run ends. A long manual ring scan with nothing else happening would leave phone admission (`BLE_PhonePipe.cpp:1048`) and glasses work refused until one of those. Not yet skeptic-checked or run on hardware. | Set the report line above the longest legitimate job (the ring gate already uses 360 s), or clear the fault when the reported run ends normally. Add a policy-table host case. Bench: `ring scan 300` with glasses up, then check `blestatus`. |
| P1-2 | G-50, G-51 [defect] | Stored XSS through filenames in the shared file explorer (`WebServer_Utils.h:342-343, 458-527`, BondFs), and the admin Settings Wi-Fi scan puts raw beacon SSIDs into `innerHTML` (`WebPage_Settings.h:2764`). No shared escaper exists and UI pages send no CSP. | One shared `escapeHtml` used at both sites, attribute-safe handlers instead of inline onclick strings; one browser test each (hostile filename, SSID `<b>x</b>`). |
| P1-3 | G-52 [defect] | First-time setup accepts a reserved owner name (system, AuthBypass, bond-admin) and boots into permanent lockout; short passwords accepted. | Reject reserved names and enforce a minimum length in `createInitialAdminUser` and both prompts; host test. |
| P1-4 | G-38, G-39 [defect] | Corrupt `settings.json` makes the device settings-read-only and blocks `factoryreset`; a corrupt owner credential file fails every login with no event or quarantine; corrupt `automations.json` reads as empty. `appendLineWithCap` retries a copy-rotation forever when there is no room; log caps total about 2.8 MB, more than the 2.23 MB and 1.81 MB LittleFS partitions on 8 MB boards; the new system log can reach 4 MB without checking free space; the SD overflow latch ignores a pulled card on SPI and on Waveshare's detect-less SDMMC. | Quarantine-and-event for the three files; rename-based rotation (the pattern b00a95dc already uses) with a free-space check; scale caps to the partition; host tests with fault injection. |
| P1-5 | G-56 [defect] | 25 bare `Usage:` replies (including the new Waveshare `speakertone`) and "Unknown feature." are stamped `OK:` and return HTTP 200, because the classifier only treats Error/ERROR as failure (`System_Utils.cpp:4682`). | Prefix them `Error:` (or classify `Usage:` as failure); update the two wifi tests that pin the bare text. |
| P1-6 | G-44 [defect] | On classic boards with Wi-Fi and HTTP up, the G2 admission gates check total free only and admit G2 at about 95.8 KB, landing near 9 KB free. | Gate on largest block plus a post-admission estimate, or declare G2 unsupported on classic boards in the matrix. |

**Before 1.0 (configuration, docs, bench):**

| Item | Groups | Problem | Bounded change |
|---|---|---|---|
| P1-7 | G-11 [defect] | Task watchdog panic is set only in `config/sdkconfig.ota.defaults`; no `esp_task_wdt_add` in the app; a hung task prints forever. Lowered from P0 because enabling panic needs a soak first (known idle-starvation paths). | Enable panic plus subscribe the main loop, in the RC soak build first; keep it if the soak shows no false trips. Soak build in place (2026-11-08): `tools/build_board.sh <board> [--config <name>] --wdt-soak`, 20 s panic, main loop subscribed; ordinary builds unchanged. See the soak note in Section 6. |
| P1-8 | G-45, G-48 [risk] | Resident stacks run with under 700 B free (`g2_owner` 312 to 688 of 6,144 B, `log_out` about 530 B and now runs rotation, `cmd_exec` 700 B); S3/classic have canary-only detection. No MSPI speed-mode sync around CPU clock changes: P4X 100/200 MHz and Waveshare 100 MHz sit below their PSRAM clocks, S3 UltraSaver runs at 40 MHz on the crystal. Separately, a 2026-11-06 P4X PSRAM "CORRUPT HEAP" (`2026-11-04-p4x-fix-progress.md:855`) is unattributed and did not reproduce under heap poisoning; the missing sync is one untested hypothesis. | Raise the three stacks to at least 1.5 KB free, measured in the soak. For clocks: either follow IDF's `clk_utils` sync rule or clamp P4 policies to at least the PSRAM clock until it is in; re-run the heap-poisoning profile under the clamped policy. |
| P1-9 | G-32, G-34 [defect/gap] | ESP-NOW PAIR_REQUEST/ACCEPT are single-shot and unacknowledged; a lost ACCEPT leaves a half pair and the timeout message and OLED flow do not offer the repair. No peer has a walk-away-and-return record; the phone scans every 30 s forever. | Retry ACCEPT and make the initiator accept a duplicate; print the `espnowpairsecure` repair hint. Walk test per stable peer in the RC bench session; phone scan backoff. |
| P1-10 | G-41, G-42, G-43, G-47 [gap] | No power-cut test of any atomic save; no structured soak for any S3 or classic profile; BLE open/close drift measured only on P4X (whose controller is the C6); no current, runtime or battery figure for any board. | Run inside the RC (Section 6). For battery, word the README as "runs from a battery; endurance not measured" unless measured. |
| P1-11 | G-07, G-58, G-59, G-60, G-03 [defect] | README lists hardware with no code (AMG8833, TEA5767, "QT PY ESP32-S3"), omits P4X-EYE, feathers3_fe and Waveshare; QUICKSTART wizard table predates the 2026-10-10 regroup and Step 4 needs `wifiautostart 1`; 18 USERGUIDE lines name commands that do not resolve; P4 local STT needs a model file no tracked artifact provides; a bare `idf.py` pairs the tracked XIAO sdkconfig with the FeatherS3 default board. | One generated board table reused by the four docs; a doc pass against the command registry (a `--check` mode for `command_registry.py` closes G-88 too); attach the STT model and `speaker.spk` to the release with their sha256; make bare `idf.py` refuse with a pointer to `build_board.sh`. |
| P1-12 | G-28, G-16, G-53, G-54 [gap] | No SECURITY.md, private reporting channel, issue templates, known-issues, troubleshooting or diagnostics-collection page, or stated security posture; no CI. | SECURITY.md with GitHub private vulnerability reporting enabled and a short posture statement (assumes a trusted LAN; BLE console unencrypted until `blesecret` is set; G2/R1 trusted by saved address; Wi-Fi PSK stored in plaintext NVS on S3/classic); one bug template asking for `bootcount`, `crashlog`, `events`, `memreport`, BUILD_INFO; KNOWN_ISSUES.md seeded from Section 7; a minimal CI job for the host suite (optional for 1.0 if the P0-4 local gate exists). |
| P1-13 | G-26 [risk] | A P4 build on a fresh machine stops before fetching esp-dl and esp_hosted, and `prepare_sdk.sh` does not check the IDF patch. The patch matters only for speaker-label builds; the S3/ESP32 worry was refuted. | Script the P4 SDK preparation into the vendor step; document that speaker labels need the patched IDF. |

### P2: explicitly defer until after 1.0

Verified and deferred (detail in Appendix A): G-15 feature defaults vs contracts, G-18 two
remaining web-UI tests (now passing on this Mac), G-20 CMake/header mirroring (Waveshare adds
three more families), G-22 links to docs a cloner cannot read, G-27 byte-reproducible images
(but publish SHA256SUMS for the 1.0 binaries, see Section 7), G-30 flash-time layout check, G-31 backup coverage (document the list now),
G-33 (fixed; needs a hardware run and a CHANGELOG line), G-35 multi-hop relay (label
experimental), G-36 transfers above 128 KB (label experimental), G-37 three-strike P4 reboot
path, G-40 salt derivation, G-46 P4X internal low-water under full load, G-49 explicit light
sleep, G-53 Wi-Fi PSK in NVS on S3/classic (document), G-54 BLE address spoofing (document),
G-55 reserved-name test gap, G-57 roster parser consolidation.

Not verified, deferred as written on 2026-10-30 (P2 groups G-62 to G-89): stale build
evidence, recovery updater version string, OTA_DEVICE_SETUP rough edges, Wi-Fi re-hunt,
the 0.99.98.4 USB-console known issue, remaining partially atomic writers, zero-length WAV
after a power cut, automations rewrite per interval, boot-automation leak and throwing
`new` sites, single httpd worker, `battery_low` from an unknown sample, the idle-downclock
GPS crash retest, dead `powerdim` (now removed), G2 case lever, secret residue in PSRAM and
an MQTT debug path that logs the device password, restore provenance, SNTP plausibility,
`factoryreset` keeping secrets, stale provenance text, tree weight, two task tables, eleven
double-registered commands, `System_Utils` catch-all, compatibility shims, `openwifi`
blocking the executor, the registry audit (exits 1 for reasons including `#if HW1_OTA_LAYOUT`
rows the parser misreads), the multi-hop docs contradiction.
Two of those deserve a look before the RC because they are cheap and user-visible: **G-73**
(`battery_low`/`battery_critical` never posted after an unknown sample) and the MQTT debug
path in **G-77** that logs the device password.

Memory slimming: stays deferred. Resume it only if the release matrix calls the bare
Bluetooth-on `xiao_s3` lane supported, which this plan does not.

SD qualification: stays deferred, with SD-backed captures labelled experimental for 1.0.
The Waveshare profile now enables SD by default, which is one more reason to label it
experimental. One bounded power-cut check during SD writes on one board belongs in the RC bench session.

---

## 5. Suggested release matrix and compatibility

| Tier | Board / deployment | Update path | What must be true at the RC |
|---|---|---|---|
| Stable | Headless Node on FeatherS3 (16 MB) | Signed OTA + cable | Clean-clone build, OTA PASS lines (P0-6), 24 h soak |
| Stable | Headless Node on Feather ESP32 V2 (8 MB, classic) | Signed OTA + cable | Same; G2 not offered on classic (P1-6) |
| Stable | Pocket Assistant on XIAO ESP32-S3 with G2 and R1 | Signed OTA + cable | Same, plus a walk-away test for G2 and R1 and a headroom figure |
| Supported, cable-only | Handheld on P4X-EYE with C6 companion | Cable only | Contract built once (G-13), 24 h soak with links and speech, STT model published |
| Experimental | Waveshare ESP32-P4 Touch LCD 5 | Cable only | Labelled experimental: about 10 days old, no deployment contract, SD on by default, user-supplied STT model |
| Experimental | QT Py ESP32 sensor appliance, FeatherS3 FE, bare `xiao_s3` with Bluetooth, generic ESP32 | Cable | No OTA record or no contract |
| Experimental features | ESP-NOW multi-hop, file transfer above 128 KB, SD-backed captures, on-device LLM, OpenClaw, CM5 co-processor, bonded mode, explicit light sleep, battery endurance | n/a | Labelled in README and USERGUIDE |

Saved configurations other than these stay available as custom builds with no support claim.
Boards stay general-purpose; the matrix is a support statement, not a build restriction.

Compatibility rows for `docs/COMPATIBILITY.md` (values from code at 95dc3f34; confirm each
against its source literal when writing the file):

| Component | Requirement |
|---|---|
| C6 companion firmware (P4X-EYE, Waveshare) | 2.12.13 exact; stock or Waveshare factory C6 needs `c6update` before radio use |
| Android app (phone link) | Phone-link protocol v2; name the minimum app build |
| Raspberry Pi / CM5 service | Protocol 1 |
| G2 glasses | Supported G2 profile versions as checked in code |
| R1 ring | The identification strings the firmware matches |
| ESP-IDF / arduino-esp32 | IDF 5.5.5 (P4 speaker-label builds need the q3 patch); arduino-esp32 3.3.5 at `11bc7ac1a` plus the tracked patches |

---

## 6. Staged execution order

Each stage has a stopping condition. Do not start a stage until the one before it meets its own.

1. **Contain (today).** Rotate the Wi-Fi passphrase and admin password; decide on the history
   rewrite. *Stop:* rotation done, decision recorded.
2. **Make it buildable and green (P0-3, P0-4).** The patch README line first, then the
   vendor script and the `build_deployment.sh` guard, then the nine harnesses and the
   RELEASING gate. *Stop:* a clean clone builds FeatherS3, Feather V2 and the XIAO profile;
   ctest is 0 failures serially and in parallel.
3. **Fix the cheap code defects (P1-1 to P1-6, plus G-73 and the G-77 password log).** Each
   gets a host test. *Stop:* all host-tested, suite still green.
4. **Write the release down (P0-1, P0-2, P0-5, P0-7, P1-11, P1-12).** Matrix, NOTICE,
   COMPATIBILITY, the `g2_worn` alias, the `c6console` name, CHANGELOG `[Unreleased]`, the
   docs pass, SECURITY.md and templates. *Stop:* every README and QUICKSTART command was run
   once against the RC build or the command registry.
5. **Freeze the branch.** Decide whether the other Mac's 84 uncommitted files (OLED CLI
   rewrite, OLED text bounds, NetworkEvents fix) go into 1.0; commit or set them aside;
   merge `codex/main-sync` (4 commits ahead) into main and push. *Stop:* one branch, pushed,
   no uncommitted work on either Mac that is meant for 1.0.
6. **Cut 1.0.0-rc.1** from a clean tree, from the clean-clone procedure, with BUILD_INFO
   and headroom recorded per matrix row and P1-7 (watchdog panic) enabled.
7. **RC bench session (P0-6, P1-9, P1-10).** One sitting per board where possible:
   - OTA cases on each OTA row: FeatherS3, Feather V2, XIAO Pocket Assistant (P0-6).
   - First install on a used board following QUICKSTART exactly, and the account-recovery procedure, once.
   - 24 h soak per stable row with a representative load: Wi-Fi + web UI polling + BLE peers
     connected (G2/R1 where offered) + periodic sensor or speech capture + logging.
     Pass criteria: no reboot or watchdog trip; no growth in `memreport` minimum free or
     largest block after the first hour beyond 5%; every stack at least 512 B free (1.5 KB
     for the three raised in P1-8); all links connected at the end.
   - Watchdog soak image (P1-7): build each stable row with `--wdt-soak`, for example
     `tools/build_board.sh feathers3 --wdt-soak`, `... feather_esp32_v2 --wdt-soak`,
     `... qtpy_esp32 --wdt-soak`, `... xiao_s3 --config xiao-no-bluetooth --wdt-soak` and
     `... p4x_eye --config p4-openclaw --wdt-soak`. Output lands in `build-<board>-wdt-soak/` or
     `build-config-<name>-wdt-soak/`; `BUILD_INFO.md` shows "Task watchdog panics: on" and the
     20 s timeout, and the boot log says the main loop is subscribed. Any `task_wdt` panic is a
     fail; record the task and backtrace. Watch for the paths not fixed in code: the P4X main
     loop's 7-11 s LittleFS stall on its first pass after setup and after a Wi-Fi join (IDLE0
     starved; 20 s is meant to clear it), long CPU-bound work on `cmd_exec` (core 0) or on core 1
     (IDLE checks), the BLE stack recycle on the main loop, and `lightsleep`. Two waits are on
     the subscribed main loop itself and are not fed on purpose, because a long one may be a real
     hang: serial `login` runs PBKDF2 inline while it holds the filesystem lock
     (`HardwareOne.cpp:2984`, `System_User.cpp:2024`), and main-loop ticks such as
     `timeAnchorsTick` wait for that lock with no timeout (`System_Mutex.cpp:76`). Any other
     task that holds the filesystem lock for over 20 s (an SD format, for example) now trips
     the main loop. Serial, UART-link and OLED commands that wait up to 60 s for `cmd_exec`
     (`openwifi` up to about 160 s) feed the watchdog while they wait. Promote panic to the release only if every row passes.
   - 20 BLE open/close cycles on FeatherS3 and Feather V2 with no per-cycle drift (G-43).
   - Walk-away-and-return for each BLE peer offered on a stable row (G-34) and `ring scan 300` (P1-1).
   - Five power cuts during settings saves, and one during an SD write (G-41).
   *Stop:* every item passes, or the failing feature moves to Experimental in the matrix with a KNOWN_ISSUES line.
8. **Promote.** If no code change was needed after rc.N, tag that exact commit 1.0.0. Any
   code change means rc.N+1 and a repeat of the affected bench items only.

---

## 7. Go/no-go checklist for the exact RC commit

- [ ] Wi-Fi passphrase and admin password rotated; history decision recorded and carried out before rc.1 was tagged.
- [ ] Tracked tree has no home paths, LAN addresses or SSIDs (host suite `tree_hygiene_gate`; SSIDs and passwords by reading the diff since the last tag).
- [ ] NOTICE.md tracked; USERGUIDE licence line fixed; LGPL decision written.
- [ ] `codex/main-sync` merged and pushed; no uncommitted 1.0 work on either Mac.
- [ ] Clean clone + vendor script builds every stable matrix row; BUILD_INFO says "clean tree" at the RC commit; headroom at or above the floor. The P4X cable-only row builds from the documented P4 SDK preparation (P1-13 is required if that row stays "supported").
- [ ] Host suite 0 failures (serial and parallel); browser suites ran; Python and OTA tool tests green.
- [ ] Supported matrix, COMPATIBILITY.md and KNOWN_ISSUES.md tracked and linked from README.
- [ ] `g2_worn` alias in place; `c6console` resolves; CHANGELOG has the full 1.0.0 entry; one version source.
- [ ] OTA PASS lines dated on the RC binaries for each OTA row, or that row labelled experimental.
- [ ] QUICKSTART followed verbatim on a used board succeeds; account recovery tested once.
- [ ] 24 h soak passed per stable row with the Section 6 criteria; watchdog panic enabled with no false trips.
- [ ] P1-1 to P1-6 fixed with host tests; or each one not fixed is listed in KNOWN_ISSUES with a workaround.
- [ ] SECURITY.md present with private reporting enabled; bug template lists the diagnostics to attach.
- [ ] STT model and `speaker.spk` attached to the release with sha256, if the P4X row ships local STT.
- [ ] SHA256SUMS published for every attached binary and bundle.
- [ ] The tag is an annotated tag on the exact commit that passed the bench (RELEASING.md lightweight-tag lesson).

### Dry run of the gate without hardware (2026-11-10)

Run on `3dedea20` plus the gate changes of the same day. This is a rehearsal, not the RC run:
the RC commit repeats all of it.

| Gate step | Result |
|---|---|
| `tools/vendor_arduino.sh --check` | OK |
| Host suite, parallel and serial | 373 of 373 passed both ways, `tree_hygiene_gate` and `esp_hosted_mempool` included. Not run: 4 browser suites (no Node.js on this Mac); 2 camera suites skipped |
| Tool tests (`tools/ota`, `tools/webui`, `tools/model`) | OK |
| Clean clone, vendor script, build | `feather_esp32_v2` only: builds, BUILD_INFO says "clean tree", no tracked file changed by the build |
| Deployment contracts, trial build (OTA rows from the clean clone with a throwaway signing key) | `headless/feather_esp32_v2`: pair builds and passes the audit, main 3,608,576 B of a 4,194,304 B ceiling. `pocket_assistant/xiao_s3`: passes, 5,378,048 B, 520,192 B free. `handheld/p4x_eye`: builds, 6,557,200 B of 0x705000. `headless/feathers3`: pair builds, audit fails: main 4,198,400 B is 4,096 B over the contract ceiling of 4,194,304 B (its `ota_0` slot is 4,718,592 B) |
| Tracked files after a deployment build | The updater lock files recorded ESP-IDF 5.5.1 and were rewritten by the build, which made the next deployment build refuse a dirty tree; fixed in c294d28e |
| Tracked tree hygiene | passes after one home subnet in the appendix was replaced |

Still open for the RC run: the browser suites, the release matrix decision (the `handheld/p4x_eye`
contract still has local speech-to-text and microSD off, unlike the builds benched since October),
the build of every stable row with the real signing key, and every line above that needs a board (OTA PASS lines, QUICKSTART on
a used board, account recovery, the 24 h soak).

---

## 8. What this assessment did not verify

- No firmware was built at 95dc3f34. All headroom figures are pre-v0.99.99 or P4-only.
- No device was touched. Every "hardware needed" line in Appendix A is pending.
- Browser suites and the phone crypto/TLS suites did not run on this Mac.
- The 28 P2 groups G-62 to G-89 were not re-verified; they are as written on 2026-10-30 at v0.99.99.
- 112 of the 333 historical ledger items were never re-checked against code
  (`20_stage2_consolidation/ledger_recheck_remaining.json`). Most concern deferred
  decisions or hardware rows, which the RC bench session would settle more cheaply than a code review.
- The other Mac's 84 uncommitted files were not assessed. If they land before the RC, re-run
  the host suite and the P1-5 and P1-11 doc checks, since they touch OLED CLI output.
- New observations from verification not assigned to a group: the unexplained 2026-11-06 P4X
  PSRAM heap corruption (see P1-8), and two persistent owners of the debug mask
  (`2026-11-04-p4x-fix-progress.md:1178-1190`; system-log autostart re-applies the saved
  `log start flags=` mask on every boot). Both belong in KNOWN_ISSUES until resolved.
- The ring-scan fault (N-1) rests on code reading only; it had no skeptic pass and has no
  Appendix A entry. Give it a host case or the bench check before fixing.
- A completeness critic (one Opus agent) checked this report against the verdict files: no
  P0/P1 group was missing; its 15 corrections (counts, line references, overstated claims,
  matrix/bench mismatches, the branch freeze step) are applied.

Appendix A: `appendix_A_verified_groups.md` (61 groups, evidence, fix, done-when).
Appendix B: `hw1-release-readiness-handoff-2026-11-06/40_report_draft/appendix_B_hardware_evidence.md` (72 dated hardware rows as of 2026-10-30).
Raw data: `verify/*.json`, `verify/ORCH-host-2026-11-06.md`, and the handoff package.
