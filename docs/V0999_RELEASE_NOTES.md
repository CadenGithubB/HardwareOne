Verified against the tree first: `PROJECT_VER` is still `0.99.89` (CMakeLists.txt:396-401, file unmodified), the rename `System_RaspberryPi.* → System_Cm5HostControl.*` is already staged, `System_Cm5Presence.cpp/.h` are untracked, `cm5/` is gone, NVS stays at `0xA000/0x4000` in every partition CSV, and all three `0x5A0000` constants in `tools/ota/` are unchanged.

---

# PART 1 — EXHAUSTIVE CHANGE LIST

123 modified tracked files, +20,671/−5,170, plus 2 untracked sources that must be added.

**File-level inventory**
- RENAMED (staged): `components/hardwareone/System_RaspberryPi.cpp` → `System_Cm5HostControl.cpp`; `System_RaspberryPi.h` → `System_Cm5HostControl.h`. Effectively a rewrite (885 lines out, 1,640 in).
- NEW, currently UNTRACKED and required to build: `components/hardwareone/System_Cm5Presence.cpp` (731 lines), `System_Cm5Presence.h` (144 lines). `System_Utils.cpp:3401` and the component CMakeLists already reference them.
- NEW, untracked, decision needed: `tools/build_coverage.sh`.
- NEW, untracked, should NOT ship: `tools/btsnoop/`, `output/`, `tmp/`, `AGENTS.md`, `2026-07-19-sdk-image-text-playbook.md`, 101 `.md` files under `docs/`.
- DELETED: none.

---

## 1. Sessions and identity — the spine of this release

Every transport (serial, UART, BLE, web, OLED, G2 lens, ESP-NOW bond) now carries a boot-local **session epoch**. A command is stamped with the epoch it was admitted under and is refused at execution if that epoch has died. This is the single largest theme and it touches roughly half the diff.

- New epoch registry in `System_User.cpp/.h`: 12 fixed slots, `transportSessionOpen/Close/EpochIsLive`, plus fixed-buffer shadows for serial and local-display so the old Arduino `String` globals are no longer read from arbitrary tasks. Those globals are now documented as "main/OLED-loop mirror only".
- `CommandContext` (`System_CommandTypes.h`) gains `transportSessionEpoch`, `authorityId`, `authoritySessionEpoch`, and three behaviour bits: `REQUIRE_LIVE_SESSION` (fail closed on a dead epoch), `MODE_INDEPENDENT` (machine traffic may not open or answer an interactive prompt), `REQUIRE_G2_EVENAI_AUTHORITY`. Several `CommandContext` members were previously **uninitialized** — all now have in-class initializers. That is a real latent bug quietly fixed.
- `executeCommand()` gained three admission fences: dead transport session (`Error: transport session changed before command execution.`), G2 EvenAI authority revalidation, and AuthBypass revalidation when a `*requireauth` setting flipped after the command was queued (`Error: authentication policy changed before command execution.`).
- **USER-VISIBLE:** new command `whoami` — "You are `<user>` (admin) on `<transport>`". Unauthenticated serial now prints `AuthBypass` rather than `(unknown)`.
- **USER-VISIBLE:** `login` and `logout` gained a target argument: `login <user> <pass> [serial|uart|display]`, `logout [serial|uart|display]`. Bare form now targets **the interface you typed it on** — previously bare `login` always minted a *serial* session regardless of caller. Targeting another session is privileged: requires a named non-Guest account, `userMayControlOtherSessions()`, a live matching epoch, and it is throttled (5 failures → 60 s lockout per caller-transport+username).
- **USER-VISIBLE BREAK:** `bluetooth` is no longer a valid `login`/`logout` target, and **`logout g2` is gone**. That was the documented way to clear the lens pairer without un-pairing. No replacement found anywhere in the tree. Flagging as possibly-collateral.
- **USER-VISIBLE:** logging out, banning, or deleting a user **no longer re-homes the G2 lens to the device owner**. `bleStampPairedByIfBlank` was removed from those paths. A lens whose pairer is banned now stops acting until re-paired. Deliberate per comments ("reconnect may restore transport, authority requires fresh pairing") but users will notice.
- New fail-closed role lookup `getUserAuthorizationRole()` replaces `isGuestUser()` in the authorization path, in web guest gating, and in the filesystem role resolver. The old helper degraded an unresolvable account to *ordinary User*; the new one returns ANON/403/denied. This closes a real hole and introduces a new failure mode: under FS-lock contention a legitimate admin can transiently degrade.

## 2. Interactive prompts are now owned by one session

`System_CLIMode.cpp` went from a single `sActiveMode` pointer to a real state machine (recursive mutex, owner `{source, epoch}`, instance id, phases Active/HandlingInput/ExitPending/Exiting, monotonic idle timeouts via `esp_timer_get_time()` — 5 min default, 10 min help, 2 min confirm, 10 min wizard).

- A mode is bound to the exact session generation that opened it. Input from any other session bypasses the mode entirely, so a foreign `yes` is just an unknown command. **A same-user re-login also cannot finish someone else's prompt.**
- `cliExitMode()` now returns `bool` and only succeeds for the owner. `onExit` is drained only on `cmd_exec_task`, outside the mode mutex.
- Confirm prompts are **no longer broadcast** — they come back as an addressed reply via new `cliConfirmPromptResponse()`. Only the OLED still gets a routed broadcast. Role rank is re-checked when "yes" arrives (`Error: session privileges changed; confirmation cancelled.`).
- Confirm payloads are now published atomically *after* mode entry is proven (new `onAccepted` callback), fixing a race where a losing request overwrote the winner's pending path. Applied to `filedelete`, `userdelete`, `factoryreset`, `ringquery raw ... status=SET`.
- **USER-VISIBLE:** machine transports — UART/CM5, MQTT, automations, G2 callbacks, and BLE — can no longer open or answer interactive prompts at all.
- **USER-VISIBLE:** ambient log suppression changed. The old device-wide gag during a help/confirm session is gone. Only the *serial* sink is suppressed, only when a serial-owned mode is up, and `[SECURITY]`/`[AUTH]`/`[ERROR]` still pass. Web console, log file, BLE and the lens now keep flowing during a help session.

## 3. Secrets stop appearing in logs, audit trails and exports

A broad redaction sweep. Several of these are genuine leaks, not hardening theatre.

- **MQTT was logging the command payload including the MQTT password.** Now logs byte count only. The response JSON's `cmd`/`result`/`error` and the `SYSEVT_REMOTE_CMD_RX` event are redacted too.
- Separately: the MQTT mesh routing path (`room:`/`tag:`/`device:`) used to call the ESP-NOW room/tag/remote handlers **directly from the MQTT callback**, bypassing authorization and command serialization entirely. It now builds a registry line and goes through `submitAndExecuteSync`.
- `redactCmdForAudit()` rewritten: token-based prefix matching (so `loginXYZ` can't dodge a rule), `isspace()` instead of literal space (a tab separator previously bypassed **every** redaction rule), `login` special-cased ahead of the table, new rules for `espnowroomcmd`, `espnowtagcmd`, `automation add`, `validate-conditions`. Peer-cred forwarding now redacts the nested command recursively.
- Redacted call sites added across: command exec traces, registry exec (which no longer prints args at all), `Unknown command:` echoes, automation COMMAND/OUTPUT autologs, G2 hijack logs (which could carry passwords), BLE login, serial login, web `/api/cli` batch results, OLED command echo and result screen.
- **USER-VISIBLE:** `testencryption` and `testpassword` no longer print plaintext, ciphertext or hash — just lengths and a verdict.
- **USER-VISIBLE:** admin session list `sid` is now an 8-char prefix (`xxxxxxxx...`), not the live cookie. `webRevokeSessionBySid()` accepts the hint form and refuses ambiguous prefixes. Any consumer storing full SIDs from `/api/sessions` breaks.
- **USER-VISIBLE, arguably a regression:** automation export and `automation list` are now redacted. An exported automation carrying credentials **no longer round-trips** — re-import silently loses the secret, with no warning at export time. Also `automation add <name>` is audit-logged as `automation add ***`, so the automation's *name* is masked and you can't tell from the log which one was created.
- **USER-VISIBLE:** `automation list` / `handleAutomationsGet` now fail closed on malformed JSON (`Error: malformed automations.json`) instead of dumping the raw file.

## 4. BLE stack, peers and reconnect

- Per-connection session epochs in `Bluetooth.cpp`; a recursive lifecycle mutex serializes the whole connection table. Epochs rotate on login, logout, revoke, idle expiry, and auth-policy change.
- **USER-VISIBLE:** BLE replies are now sent point-to-point (`esp_ble_gatts_send_indicate` at the connection). The old multi-client behaviour — prefix `[ble conn:N]` and notify characteristic-wide, so **every peer saw every reply** — is gone.
- **Security fix:** BLE login used to re-serialize into `login <u> <p> bluetooth` and hand it to the command registry. A crafted `login u p display` could be interpreted as a request to replace the **OLED** session. Login is now a native, quote-aware, epoch-bound path with the password zeroed on free. Failed BLE login now always returns `[ble] Authentication failed.`
- New BLE role-transition transaction (`SERVER_START/STOP`, `G2_START/STOP`, `RECOVERING`) with a latched lifecycle fault. Starting the server now quiesces the Ring owner and normalizes the client host rather than doing `btStop()/btStart()` under a live Bluedroid. New console strings for deferred/blocked/incomplete transitions, several ending in "reboot required".
- `isBLERunning()` split into `isBleControllerEnabled()` / `isBluedroidHostEnabled()` / `isBleServerInitialized()`. **USER-VISIBLE:** `bleSubsystemActive()` no longer reports true for a bare enabled controller, so status surfaces that previously showed BLE "up" with no role now show it down. `/api/system` `bt.server` changed source accordingly.
- Peer registry rewritten: `gBlePeerData` is now private behind snapshot accessors; `pairedByUser` became a real owner *session* with generation + transport epoch; connect became an admission protocol returning `STARTED/COALESCED/BUSY/ALREADY_UP/NO_TARGET/ROLE_BLOCKED` with per-result backoff.
- **USER-VISIBLE BEHAVIOUR CHANGE:** the "legacy heal" that silently stamped `pairedByUser = device owner` for any peer with a saved MAC is **deleted**. A device whose `settings.json` has a MAC and `autoReconnect:true` but no `pairedByUser` will no longer bring BLE up at boot and will never auto-reconnect. It logs `Skip boot auto-reconnect '<peer>' — owner authority unavailable`. `bleautoreconnect on` now refuses with a new message pointing you to log in first.
- G2 central role: client retirement registry replaces three hand-rolled "poll GATTC then delete or leak" blocks; a per-connect `new G2ScanCallbacks()` **leak** is fixed; connect now fails closed on a missing write char / non-notifiable notify char / failed `registerForNotify()` rather than accepting a mute link.
- **USER-VISIBLE:** `g2recover` is now asynchronous — "G2 recovery: missing-temple repair queued — use g2status to watch".
- **USER-VISIBLE:** `g2evenai` and `g2status` output rewritten with CM5 host-gate detail; the single `host_link_lost` reason became 8 distinct ones.
- Depends on patched Arduino-BLE APIs (`deinitChecked`, `deleteClient`, `BLERemoteNotifyResult`, `getLastConnectOutcome`, `connectStageToString`). `docs/arduino-local-patches/arduino-local-patches.patch` is regenerated (+2023/−78) to carry them.

## 5. R1 ring

- **New RING debug-flag family** (8 flags) and a UART family (3). 11 new commands (`debugring*`, `debuguart*`), 11 new persisted settings. **USER-VISIBLE:** `ringverbose` **no longer exists** — raw hex dumps moved under `debugringdump`, default off. ~15 unconditional broadcast lines are now level-gated `[INFO][RING]`/`[WARN][RING]`/`[ERROR][RING]`.
- **Multi-notification fragment reassembly** for activity-daily. Daily record cap 35 → 144 (a full day). New reassembly buffer in PSRAM, CRC-verified against the whole model. `fullDayVerified` now flips **false → true** — a semantic change every downstream health consumer sees.
- **USER-VISIBLE:** opening the Activity screen now actually arms a history sweep (previously it fetched nothing), "Poll Now" refreshes trends too, and the empty state gives a 4-way diagnosis instead of the misleading "Refresh after ring setup".
- **USER-VISIBLE:** `ringscan` is now asynchronous — returns "RING: Ns scan queued — use ringstatus, then ringconnect" and no longer reports results inline. `ringstatus` gains `pending=N` / `connectPending`.
- **USER-VISIBLE:** `ringconnect` while a G2 audio session is live now **waits up to 90 seconds** instead of declining. It also no longer blocks on a merely-open mic — only a real recording or SR ownership counts.
- Dark-boot deadlock fixed: the ring setup TIME stage waited for a clock the tick would only solicit *after* setup finished. New in-setup probe; if the ring answers with a pre-2020 stamp we complete with our own dark epoch instead of dropping the link.
- Timezone re-push added — a pure timezone edit now propagates to the ring (previously only a >120 s drift did).
- Ring RX transport moved to PSRAM slabs (~3 KB internal DRAM reclaimed) with an ownership state machine; a boot self-test now runs and **disables the entire ring module on failure**; a hard `#error` if external BSS is not enabled.
- Real bug fixed in the ring self-test: a `memcpy` read one byte past a string literal, which on an unlucky nonzero byte would have **silently disabled the ring module at init**.
- New `R1PacketAckDescriptor` capability type — ACKs can only be minted from validated wire data, and the pending-queue element shrank from ~290 B to 12 B.

## 6. CM5 co-processor

The CM5-side daemon has moved out to its own repository. What remains here is the firmware half.

- **USER-VISIBLE BREAKING RENAME:** `hostpower …` → `cm5 power …`, and the EVT wire tokens renamed `hostpower_*` → `cm5_power_*`. Any script or automation using `hostpower` breaks. The protocol version string is still `"1"` despite the rename.
- New `cm5 fan [show|status|quiet|auto|max]` protocol with temperature, PWM, RPM and health reporting, independent of power. A `max` request may supersede a pending non-`max` one.
- New presence lease: `cm5 heartbeat` handled as an allocation-free UART control-plane intrinsic that never touches the command queue or history. Normal lease 15 s, `busy` 75 s. A small dedicated task owns the fresh/stale edge and wakes the G2 EvenAI host gate.
- New CM5 time anchor: `cm5 time set` stashes an epoch which the main loop applies. Dark boot adopts it; an already-synced clock is only corrected if the Pi is NTP-synced, the current source is neither manual nor NTP, and drift exceeds 120 s. New sync source `cm5` appears in clock status and the `time_synced` event.
- **USER-VISIBLE:** `uartlink` status line grew — now reports epoch, last epoch, last event, cm5 freshness/age/task.

## 7. WiFi radio

- New radio arbiter (`WifiRadioMutationGuard`) and a new `wifiScanForEach()` that uses IDF directly and **releases the driver AP list before returning**. Arduino's global scan buffer no longer stays allocated while a wizard waits on a human.
- **USER-VISIBLE:** new busy errors instead of silent half-completion — `Error: WiFi radio busy (scan or connection in progress); retry closewifi`, and `radiopower off` now returns an explicit error and retains its restore snapshot rather than half-powering-down.
- **USER-VISIBLE:** scan failures now surface a reason everywhere instead of "No networks found" — `wifiscan --json` returns a valid document with `error`/`driverError`; the web settings page renders `Scan <error>; retry shortly.`; the OLED shows `Scan <status>; try again`; the G2 network page shows `(scan <status>; retry)`.
- `closewifi` reordered so a busy radio leaves the web server usable.

## 8. OLED

- The local display is now a single-owner render loop. Auth changes arriving on `cmd_exec` no longer synchronously mutate keyboard state, navigation or the framebuffer — they set an atomic and the main loop applies the boundary. Eight new per-mode session-reset hooks wipe keyboard buffers, credentials and navigation on identity change.
- New framebuffer commit fence: a frame drawn for epoch N is never pushed after epoch N+1 is published.
- **USER-VISIBLE:** typing `login`/`logout` into the OLED CLI keyboard is refused with "Use the OLED Login screen" / "Use the OLED Logout screen".
- **USER-VISIBLE:** the "Login successful!" and "Logged out: `<user>`" confirmation screens no longer appear.
- **USER-VISIBLE:** `oledstop` on an absent panel now says "OLED display not running; session cleared" and actually clears the session (prevents a ghost session being inherited by a later hot start).
- **USER-VISIBLE:** the settings-editor quick Bluetooth toggle is G2-aware — dispatches `g2deinit`/`openg2` in G2 client mode instead of always `openble`/`closeble`.
- First-time-setup WiFi picker rewritten: recursion eliminated (repeated rescans previously grew the stack), stack `String[20]` replaced by PSRAM caches wiped on exit, SSID read from the record instead of parsed back out of the display label, and Rescan/Manual matched by index so an SSID named `< Rescan WiFi >` can no longer spoof them.
- Build fix: `System_MemUtil.h` and `<new>` were inside an `#if ENABLE_ESPNOW` block in the file browser while being used ungated — an ESP-NOW-off build did not compile.

## 9. Web server

- Session epoch sidecar with a fixed SID mirror; every server stop invalidates all epochs, every start republishes live ones.
- **USER-VISIBLE:** session reuse is gone — every successful login mints a fresh SID, so a browser holding the old cookie cannot inherit the new login's mode ownership.
- **USER-VISIBLE API CHANGE:** `/api/cli` and `/api/cli/batch` accept a new `interactive` flag. Without it, commands are submitted as machine traffic that cannot answer a confirmation. Interactive batch is restricted to exactly `[command, "yes"]` and requires a real cookie session (Basic Auth is rejected). Results are epoch-checked at four points; a dead session returns 401 `web_session_changed` and **discards accumulated batch results**.
- The web terminal now suppresses its 500 ms log poller while in help, so help output is no longer clobbered.
- Bond CLI batch gained interactive confirmations for `filedelete`/`userdelete` only (translated to the one-shot ` confirm` form, since a CLI mode cannot span web → ESP-NOW → peer). **USER-VISIBLE:** success now reports "Pending: remote deletion accepted for delivery; verify the bonded device state." — correctly acknowledging that wire delivery is not command success.
- Web guest gating no longer string-compares the username; it resolves the real role and 403s if it cannot.

## 10. ESP-NOW

- `closeespnow` is now a real, joinable, memory-releasing shutdown via two spinlock-protected admission counters wrapping every first-party send path, failing closed with "ESP-NOW closing" instead of leaking.
- Credential/secret redaction across every ESP-NOW logging, storage, event and response path, plus a wire-level encryption tightening.

## 11. Notifications, events, boot

- **USER-VISIBLE:** new Android companion-app notification sink. New setting/command `notifydeviceapp` (default on). Cards go over the ordinary BLE reply lane as a single `#NOTIF {json}` line, capped at 195 bytes so it is provably one frame. Only authenticated BLE sessions receive them, each judged against its own logged-in user's importance floor and mutes. New `notifstats` counters. **The companion app must be updated to reject `#NOTIF` before its capture-buffer append** — documented in `docs/APP_JSON_CONTRACT.md` §9.
- **USER-VISIBLE, BREAKS AUTOMATIONS:** the `boot` event split into `boot_started` (posted early, subject = reset reason) and `boot_finished` (last statement of setup, detail = `boot #N`). `"boot"` is kept as a read-time alias for `boot_finished` so stored configs don't orphan, but the *emitted* name changed and the web automations picker now offers the two new names.
- **USER-VISIBLE:** `taskstats --json` gained an optional `stackSize` field (bytes), from a new creation-time task-stack registry — deliberately absent for IDF-owned tasks so consumers render headroom-only.

## 12. Microphone, camera, live audio

- **USER-VISIBLE FIX:** recordings stop losing word tails. The single silence threshold was doing two jobs; discard now uses a floor-relative threshold while auto-stop timing is unchanged. Measured: "potato" → "potat", "picture" → "pict".
- **USER-VISIBLE FIX:** mic source latch. `micsource` could report `preference=g2, active=pdm` for a whole session with the glasses connected, producing −25 dBFS audio and bad transcripts. Recording start now re-resolves the source and restarts the mic if it disagrees; if the restart fails, the start is reported as failed rather than claiming a recording with no capture.
- Camera power worker rewritten around a desired-state latch and a completion-slot pool (replacing stale task-notification handles). **USER-VISIBLE:** `camerastop` can now return "Error: Camera stop failed or timed out"; `camerares`/`cameraframesize` can return "Error: Resolution saved, but camera restart failed". ~30 command buffers moved to PSRAM.
- `liveaudio ready` renewal became an allocation-free UART fast path (renewal only — it refuses to create or resurrect a lease). `liveaudio capabilities` gains `renew_direct=1` and prints real TTL constants instead of hardcoded values.

## 13. Filesystem and first-time setup

- **New recovery:** a LittleFS mount failure on a *provably blank* partition (all 0xFF, scanned via `esp_partition_read`) now formats once instead of bricking the boot. `esptool erase_flash` previously left a board that could not boot at all. Corruption of real data still fails closed.
- **Lockout fixed:** first-time setup wrote `users.json` (the "setup complete" token) *before* the admin credential. A power cut between them produced a device that reported setup-complete, rejected every login, and never re-ran setup — reflash only. Both sites now write the credential first and `users.json` last, only on success.
- **USER-VISIBLE:** new error `ERROR: credential not stored — users.json withheld; setup will re-run on next boot`.

## 14. Build, partitions, tooling

- **All five partition tables grow the app partition by 512 KiB at LittleFS's expense.** App headroom was ~2%. Because the data partition offset moves, LittleFS is reformatted on first flash. NVS is untouched.
- `CONFIG_ESP_IPC_TASK_STACK_SIZE` 1024 → 1536 for all boards (the S3 carves the AI-coprocessor context save area out of every task stack). ~1 KB internal DRAM.
- `boards/xiao_s3.defaults` pins the camera sensor allowlist to OV2640/OV3660/OV5640 and explicitly disables 16 other drivers whose register tables sit in internal DRAM permanently. **USER-VISIBLE only if a non-OV sensor is attached to a XIAO Sense — it will no longer be detected.**
- `ENABLE_G2_GLASSES` is now forced to 0 when Bluetooth is off. It was a never-`#undef`'d literal, so ~30 bare `#if ENABLE_G2_GLASSES` sites silently evaluated true on BT-off boards.
- `tools/command_registry.py` widened to accept multi-word command keys so generated docs list `cm5 status` etc.
- G2 hijack FSM worker stack 2560 → 4096 bytes (measured 2280 B peak, only 10.9% headroom).

---

# RISKS AND THINGS TO CHECK BEFORE APPROVING

### Blocking — the commit is broken or wrong without these

1. **`PROJECT_VER` is still `0.99.89`.** `CMakeLists.txt:396-401` is unmodified. A v0.99.9 release commit must bump it, or every device reports the old version.
2. **`components/hardwareone/System_Cm5Presence.cpp` and `.h` are untracked.** `System_Utils.cpp:3401` registers `cm5PresenceCommands` and the component CMakeLists lists the source. A clean checkout will not build without them. They must be `git add`ed.
3. **`tools/btsnoop/` must not be committed** — HCI capture and reverse-engineering tooling, including a 33 KB `ota_extract.py`. Also untracked at repo root: `output/`, `tmp/`, `AGENTS.md`, `2026-07-19-sdk-image-text-playbook.md`, and 101 `.md` files under `docs/`. **Do not use `git add -A`.** Stage explicitly.
4. **`tools/build_coverage.sh` needs a deliberate decision.** It is genuinely useful (two-pass compile coverage for board-gated code) but it **mutates the live `System_BuildConfig.h` while running**, restoring it via an md5-verified EXIT trap. If you flip build flags by hand while it runs, the trap is the only protection.

### Destructive / migration

5. **Every board loses its LittleFS data on the first flash after this.** Expected and documented in the CSVs.
6. **The two SR layouts also relocate the `model` (spiffs) partition** — the ESP-SR model blob must be re-flashed. The new warning block only mentions LittleFS, so the warning is incomplete for SR boards.
7. **OTA cannot deliver this.** Field devices still hold the old table with `ota_0 = 0x5A0000`; an app-only OTA does not rewrite the partition table. This build needs a cable flash. Nothing in the diff addresses that.
8. **OTA tooling was not updated** — `tools/ota/check_ota_builds.py:25`, `tools/ota/make_bundle.py:40`, `tools/ota/make_manifest.py:350` all still hardcode `0x5A0000` and will mis-size or reject images the new table permits. Verified unchanged in the tree.
9. **The blank-partition LittleFS recovery probably does not fire on a flash-encrypted board.** `littlefsPartitionIsBlank()` uses `esp_partition_read()`, which transparently decrypts; an erased region decrypts to non-0xFF and the probe returns false on byte 0. `esp_partition_read_raw()` is the encryption-agnostic call. Given flash encryption is enabled on the bench FeatherS3, worth verifying.
10. **Saved `sdkconfig` is out of sync with `boards/xiao_s3.defaults`** — it still has `CONFIG_OV7670_SUPPORT=y` / `CONFIG_GC0308_SUPPORT=y`. A build reusing the committed `sdkconfig` gets none of the camera-driver DRAM saving.

### Behaviour changes that will surprise users

11. **Existing `settings.json` peers stop auto-reconnecting** if they have a saved MAC but no `pairedByUser`. Intentional per comments, but silent.
12. **`logout g2` removed** with no replacement. Intentional or collateral?
13. **`hostpower …` → `cm5 power …`** breaks any existing script/automation. Also `boot` → `boot_started`/`boot_finished` breaks automations triggering on boot.
14. **Automation export no longer round-trips** credentials, with no warning at export time.
15. **`ringverbose` removed but still documented** — `System_Utils.cpp:3449` and `docs/COMMAND_REFERENCE.md:1245,1254` still describe it.
16. **OLED Login removed from the G2 Config menu** (~70 lines deleted, `G2_CONFIG_MAX_ROWS` 4 → 3). `docs/USERGUIDE.md` documents the removal, but the diff itself states no rationale.
17. **Ring connect watchdog no longer self-heals.** A stuck central job used to clear after 240 s; it now permanently blocks ring admission and logs "reboot may be required".
18. **A 90-second ring audio-defer can occupy the single shared BLE-connect worker**, delaying queued G2 jobs.

### Latent bugs / performance worth a look before shipping

19. **`oledIsGuestSession()` now does a `users.json` open + full JSON parse on every call.** It is called from `oledGuestBlocksMutate()` at the top of ~20 input handlers and from menu rebuilds. **Every OLED button press now costs a filesystem read.** This is exactly the shape of the previously-diagnosed ~570 ms input stall. Strongly recommend caching the role and invalidating it on the session epoch this same changeset already provides.
20. **`OLED_SettingsEditor.cpp` has no session-reset hook** despite the boundary comment claiming every keyboard-backed mode does. A `SETTINGS_VALUE_EDIT` + keyboard state survives the boundary, after which the handler returns "keyboard still active — central dispatch owns input" while the central keyboard is *not* active. Plausible input wedge for the replacement identity. `OLED_Mode_LLM.cpp`, `OLED_ESPNow.cpp`, `OLED_Mode_Map.cpp`, `OLED_Mode_Remote.cpp` are also uncovered.
21. **~4.6 KB of new permanent internal DRAM** — two function-local `static R1ActivityDailyResult` buffers in `System_R1_Protocol.cpp:1253` and `:1419`, neither `EXT_RAM_BSS_ATTR` unlike the ring-side scratch. The self-test one is used exactly once at boot. Looks unintentional.
22. **`setSession()` can now return an empty String** (lock unavailable / epoch publish failed) and **two of its three call sites ignore the return** (`WebServer_Server.cpp:3896`, `:3950`). Behaviour on that path — login "succeeds" with no session — is unverified.
23. **The `gCLIState = CLI_NORMAL` reset after a `capture=1` API call was deleted.** Its comment warned that without it, `help` via the API sets `gCLIState` globally and breaks subsequent commands from any source. The new CLIMode ownership is presumably the replacement, but that is an assumption. Verify `help` via `/api/cli` on hardware.
24. **`deinitBluetooth()` can bail out *after* killing all session epochs.** If Ring quiesce or host teardown fails it returns early with the server still up and live connections whose epochs are 0 — those clients are permanently mute with no message, until they physically reconnect.
25. **`cameraPowerRequestStopAsync()` lost its no-op guard.** A stop on an idle Sense build now spawns a ~10 KB task just to no-op (and returns false if that allocation fails, where it used to return true).
26. **`executeCommand()` now returns `registrySuccess` instead of unconditional `true`.** Every caller keying off that bool now sees failures it previously did not. No caller sweep was done.
27. **Bonded forwarding now applies local authorization** to the unwrapped command. This will break master/worker pairs where a local non-admin drove admin commands on the peer.
28. **`AtomicFlagGuard` in `System_User.cpp` is a busy-wait spin with no priority inheritance.** A high-priority waiter can spin against a preempted holder on the same core. Related: `serialTransportSessionClearAndBeginDelivery()` returns with the writer flag **still held** and relies on a caller in `HardwareOne.cpp` to release it; a violation is a hard spin, not a timeout.
29. **BLE brute-force lockout may no longer apply.** BLE login moved off the command registry to `validateTransportCredentials` + `recordTransportLoginResult` directly, with no visible `isLoginLocked`/`recordFailedLogin` call — whereas the parallel serial rewrite explicitly kept them. Worth verifying.

### Debug-only / scaffolding that should probably not ship

30. **Raw EvenAI EVENT hex dump** left in `G2_Glasses.cpp` `handleEnvelope` — dumps the entire event body plus a flat protobuf log on every event, explicitly labelled as an RE aid for a follow-up gesture investigation. Gated by `debugg2`, but unconditional within it. Decide before committing.
31. **`DEBUG_RING` was added to `kBootDefaultDebugFlags`** while every `debugRing*` setting defaults false. Because sub-flags gate on parent-OR-sub, the parent being on at boot turns on *all* ring traces. Confirm on hardware that `debugflags` doesn't come up loud.
32. **Fragment reassembly framing is reverse-engineered and unverified on hardware** — the finalize-reject path exists purely as wire-RE diagnostics. Expect a hardware iteration.
33. **`fullDayVerified` flips false → true.** Check every downstream health consumer (OLED, web, JSON) before shipping.

### Cosmetic / cleanup

34. **Dead code introduced:** `isHelpModeCommand()` (zero callers), `isBLERunning()` (zero callers), `bleSavePeerMac()` (zero callers), and three exported-but-uncalled new peer APIs (`blePeerNoteUserConnectIntent`, `bleSavePeerMacIfIdentityCurrent`, `blePeerNoteLinkUpIfIntentCurrent`). Also the OLED logout-confirmation screen (`logoutMessageUntil`/`loggedOutUser` are now only ever assigned zero/empty — render and dismiss blocks are unreachable). Unresolved whether that screen's removal was intentional.
35. **Stale comments contradicted by this diff:** three partition CSVs still say "offsets are UNCHANGED" and list the old sizes; root `CMakeLists.txt:131-134` lists old partition sizes; `System_BuildConfig.h` still references `hostpower`/`hostfan` command names; flag-count prose in `System_Debug.cpp:2252` and `System_Settings.cpp:2032,1764` is off by 8 (the asserts are computed, so nothing breaks).
36. **`WebServer_Server.cpp` `setSession()`** — ~50 lines were wrapped in a new guard block without re-indenting. Compiles, reads badly.
37. **`System_Cm5PresenceCM5.cpp` is added to the CMake source list with no feature gate**, unlike everything else CM5-related. Costs flash on every board including ones with no CM5. Confirm intended.

---

# PART 2 — THE COMMIT MESSAGE

```
v0.99.9: signing in is now tied to the interface you signed in on, secrets stop leaking into logs and exports, and the firmware gets 512K more room to grow

New

+ Every way into the device — serial, USB host link, Bluetooth, the web
  page, the little screen, the glasses — now carries its own session,
  and a command is refused if the session that admitted it ended before
  it ran. Logging out no longer leaves queued work to finish under your
  name after you're gone.
+ Confirmation prompts belong to the session that asked. There was one
  device-wide "waiting for input" slot with no record of who opened it,
  so a "yes" arriving from anywhere — another terminal, the web page,
  an automation — answered whatever delete prompt happened to be open.
+ "login" and "logout" take a target: "login alice hunter2 display",
  "logout serial". Bare "login" now means the interface you typed it on
  rather than always meaning serial.
+ "whoami" tells you who the device thinks you are, and where.
+ The Android app can receive notification cards straight from the
  device over its existing Bluetooth link, honouring each user's own
  notification levels and mutes. Needs a matching app update.
+ The ring gets a full day of activity history instead of a truncated
  slice, its own debug-flag family, and quiet logs by default.
+ New "cm5 fan" for temperature, speed and health, a presence heartbeat
  so the firmware knows when the Linux side is alive, and a clock
  handoff that can set the device time from the Pi.
+ The web API's /api/cli takes an "interactive" flag, so a page can
  answer a confirmation while machine traffic still can't.
+ "taskstats --json" reports each task's stack size, not just headroom.
+ Scan failures now say why — on the web page, the little screen, the
  glasses and in JSON — instead of "No networks found".
+ 512K more room for the firmware itself.

Fixed

- MQTT was writing whole command payloads to the log, broker password
  included. It logs a byte count now, and audit lines, command history,
  the web terminal and the on-screen console all show a redacted form.
- A Bluetooth client could open a session on a different interface.
  Sign-in over BLE was re-typed into "login <user> <pass> bluetooth",
  but the password was taken as the entire rest of the line — so
  "login alice hunter2 display" came out as a login for alice with the
  target set to the local screen. With real credentials, a remote
  client could mint a screen or serial session with nobody at the
  device. Sign-in is now a native, quote-aware path with no re-typing.
- Automations stop printing their own secrets. An automation whose
  action is a command carrying a password had that line shown verbatim
  by "automation list" and the web automations API. Both redact now.
- Guest and unknown accounts fail closed instead of quietly getting
  ordinary-user access.
- Bluetooth replies go to the one client that asked. With more than one
  app connected, every client used to see every reply.
- The ring reconnects from a cold boot, re-syncs when you change
  timezone, and its Activity screen actually fetches history instead of
  sitting empty.
- Bluetooth and WiFi stop fighting each other and themselves. Scans,
  connects and power-downs are serialized, and a busy radio says so
  instead of half-finishing.
- Recordings keep the ends of words.
- The microphone no longer latches onto the wrong source while the
  glasses are connected, which was producing very quiet audio and bad
  transcripts for a whole session.
- Erasing the flash used to leave a board that couldn't boot at all. A
  blank data partition is now formatted once on first boot.
- A power cut during first-time setup could lock you out permanently.
  Setup writes the password before it marks itself complete.
- An ESP-NOW-off build didn't compile.

Breaking

- "ringverbose" is gone — use "debugringdump". "logout g2" is gone. The
  OLED Login row left the glasses Config menu.
- "hostpower" is now "cm5 power".
- The boot event split into "boot_started" and "boot_finished" — update
  any automation that watched for it.
- Copying an automation back out of "automation list" or the web
  automations API no longer carries its password — the line comes back
  redacted, so it won't work if you paste it in again.
- A saved Bluetooth peer with no recorded owner won't auto-reconnect
  until someone pairs it again.
- The Linux co-processor software now lives in its own repository. What
  stays here is the firmware side.
```