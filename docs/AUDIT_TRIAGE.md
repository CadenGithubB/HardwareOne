# Audit Triage — Corrected Priority List

> ## ⛔ CORRECTION 2026-08-03 — this document refuted findings on a wrong build flag
>
> The trust-calibration paragraph below states that the ESP-NOW and web themes "lost multiple HIGH
> entries to a single missed build flag (`ENABLE_BONDED_MODE 0`, `ENABLE_WEB_BOND 0`)".
>
> **`ENABLE_BONDED_MODE` is `1`, not `0`** — verified directly at `System_BuildConfig.h:320`. Its only
> demotion is behind `#if !ENABLE_ESPNOW`, which does not fire (`ENABLE_ESPNOW` is 1). So
> `#if ENABLE_ESPNOW && ENABLE_BONDED_MODE` at `System_Utils.cpp:4669` **is compiled**, and every
> refutation in this document that rests on bond code being absent is itself wrong. Re-instate those.
>
> `ENABLE_WEB_BOND 0` (`:89`/`:565`) is correct — `/api/bond/*` really is not compiled.
> Also confirmed while checking: `DISPLAY_TYPE 0` → `ENABLE_OLED_DISPLAY 0`, so OLED senders are not
> compiled either; `ENABLE_MQTT 0`.
>
> **Established by the same follow-up trace** (2026-08-03, prompted by the project owner correcting a
> wrong conclusion of mine): the `remote:`/`@` privilege gap is REAL and is *not* limited to a
> physically-present operator. `ESPNOW_V4_TYPE_CMD` is registered `V4_OPC_FLAG_REQ_PAIRED` only, and
> `v4_handle_cmd`'s `username:password:command` branch queues the line verbatim — so a merely-*paired*
> peer holding an ordinary user account on this device can send `user:pass:@<supercmd>`, have
> `authorizeCommand` fail open (`findCommand` cannot resolve the prefix; `System_Utils.cpp:3377`/`:3392`
> both `return false`), and get it executed on this device's bond peer as `kBondAdminUser` = super —
> without ever holding a credential on that peer. Bounded by requiring a live bond on the forwarding
> device (`bondModeEnabled` defaults false; arming it is admin-gated).
>
> **Lesson for reading the rest of this file:** refutations that turn on "this code is not compiled"
> were not independently verified against `System_BuildConfig.h`. Treat that specific class of
> refutation as unproven until checked.

**No code was modified.** This document is the only file written by this pass. No source file, build file,
or git state was touched.

**Posture.** The firmware's security model is coherent in design but is defeated in practice by a single
recurring pathology — an authorization or redaction decision is made against one representation of a
request while a *different* representation is what actually executes or persists (the `remote:`/`@` wrapper,
the non-canonical path, the per-fragment ESP-NOW auth verdict, the confirm-mode `yes`) — and by a second
pathology in which filesystem and hardware writes are treated as infallible, so a full or failing flash
silently commits truncated credential and settings files while reporting `OK:`. Stability is dominated by
unbounded `snprintf` accumulators writing past small stack buffers on callback tasks, and by sixteen boot-path
`while (1)` wedges that no watchdog on this build can end.

**How much to trust the raw findings.** 505 findings were re-verified; 41 did not survive and 51 needed
severity correction. The refutation rate is not uniform, and two themes should be read with real skepticism:
the **ESP-NOW** theme and the **web** theme both lost multiple HIGH entries to a single missed build flag
(`ENABLE_BONDED_MODE 0`, `ENABLE_WEB_BOND 0`), and several entries labelled their board field `all` for code
that is not compiled on the shipping FeatherS3 target. The **build-config/boot/docs** theme has correct
conclusions attached to stale line numbers and one prescribed fix that does not work. The **BLE/G2** and
**users/auth** themes verified cleanly on mechanism and are the most trustworthy. Across every theme the
consistent weakness was *reachability*, not mechanism: finders traced code accurately and then asserted
impact without checking whether the attacker can reach it, whether the file is compiled, or how narrow the
precondition window is.

**Two corrections found during this triage pass** (neither was in the specialists' output):

- `writeTextAtomic()` has **already been hardened** — System_Utils.cpp:840-846 explicitly refuses a
  truncate-in-place fallback on rename failure, with a comment naming that exact hazard. The surviving
  defect in that cluster is strictly the unchecked `f.print(in)` inside `writeText()` (:813-825). Any
  write-up implying `writeTextAtomic` shreds the destination on rename failure is describing
  `writeSettingsJson`, not this function.
- `normalizeFsPath()` (System_Filesystem.cpp:1458) rejects more than the CRITICAL's write-up credits: it
  rejects `..` anywhere, rejects literal double-quotes, and rejects control characters. The bypass is
  therefore **specifically the `.` segment**, which is neither rejected nor collapsed, plus the absent
  leading-slash enforcement. See item 1 for what this does and does not change.

---

## 1. Corrected top 15

Ranked by severity × confidence × blast radius × ease of exploitation. Everything here is compiled and live
on the shipping FeatherS3 build unless stated.

| # | Finding | Location |
|---|---------|----------|
| 1 | Permission check and I/O canonicalize differently — non-canonical path skips every PathRule | System_Filesystem.cpp:1458 / :1396 |

**What breaks:** `lookupRule()` is a bare `startsWith` over a table whose every row begins with `/`, and its
last row is the catch-all `{nullptr, PERM_ALL, PERM_ALL, PERM_ALL}` (confirmed at :1391). `normalizeFsPath`
does not collapse `.` segments and does not force a leading `/`, while `VFS::normalize()` on the I/O side
does prepend `/`, and littlefs skips `.` components outright. So `/./system/users/users.json` matches no rule,
falls to `PERM_ALL`, and writes the live account database — for any role, from serial, web CLI, BLE, G2, OLED
and ESP-NOW. **Scope correction:** `requireQuotedPath` (:1508) *does* prepend `/`, so the bare
`system/users/...` form is closed on quoted-path CLI commands; the `.`-segment form is the universal one.
Whether `handleFileWrite`'s `name=` parameter reaches the rule table without a leading slash needs one more
read of WebServer_Server.cpp:1739-1852 before the "any web user in one request" claim is repeated.

| # | Finding | Location |
|---|---------|----------|
| 2 | Full 32-hex session SID broadcast into the web-readable log feed | WebServer_Server.cpp:322, :376 |

**What breaks:** `BROADCAST_PRINTF("[auth] setSession user=%s, sid=%s, ...")` emits the complete bearer token.
Confirmed at System_Debug.cpp:246: the web-mirror sink tests `(msg->routing & MSG_ROUTE_WEB) && gWebMirror.buf`
— it does **not** consult `gOutputFlags`, unlike the serial sink (:242) and file sink (:253). The per-user
output-flag gate that suppresses serial and file does not suppress the web mirror. `GET /api/cli/logs` admits
any non-guest account. A low-privilege user polls one endpoint the CLI page already polls every 500 ms and
replays the admin's cookie. Fix already exists 40 lines away — the `%.8s...` truncation in `authSuccessUnified`.

| # | Finding | Location |
|---|---------|----------|
| 3 | Pre-authentication one-byte stack write in `decodeBasicAuth`, ahead of the lockout | WebServer_Server.cpp:1346-1350 |

**What breaks:** Confirmed verbatim — `unsigned char out_buf[256]`, `sizeof(out_buf)` passed as `dlen`, then
`out_buf[out_len] = '\0'` with no bound test. A 344-char base64 header decodes to exactly 256 and the
terminator lands on `out_buf[256]`. In `isAuthed` the decode runs *before* `isLoginLocked` and before
`isValidUser`. With `-fstack-protector` the likely landing site is the canary: an unauthenticated client on
the network gets a repeatable remote reboot loop against any `tgRequireAuth` URI, unthrottled.

| # | Finding | Location |
|---|---------|----------|
| 4 | Unauthenticated ESP-NOW `METADATA_PUSH` → unescaped `innerHTML` → admin-privileged CLI | WebPage_ESPNow.h:739, :780 |

**What breaks:** `METADATA_RESP`/`METADATA_PUSH`/`HEARTBEAT` are registered with `flags = 0` — no pairing, no
session, no auth — and fingerprint 0 is explicitly waved through as "no mesh scope". Attacker-chosen
`friendlyName`/`room`/`zone` is `strncpy`'d into `gMeshPeerMeta` and concatenated into HTML. Someone in radio
range with no account and no mesh key gets same-origin JS in the admin's browser on the next ~10 s poll, which
POSTs to `/api/cli` at admin privilege. Live: `ENABLE_ESPNOW=1`, `ENABLE_WEB_ESPNOW=1`.

| # | Finding | Location |
|---|---------|----------|
| 5 | ESP-NOW `FS_LIST`/`FS_STAT`/`FS_GET` serve the whole filesystem under SYSTEM identity | System_ESPNow_FsList.cpp:644, 784, 842, 896 |

**What breaks:** The six `FS_*` dispatch rows sit *above* the `#if ENABLE_BONDED_MODE` block and carry only
`REQ_PAIRED|REQ_SESSION_ENC`. Handlers install `SYSTEM_IDENTITY_SCOPE`, and `isUnrestrictedRole(SYSTEM)`
exempts the request from the sensitive-extension guard. A peer holding **no account on this device** pulls
`/system/users/users.json`, `/system/certs/*.key` and `/system/espnow/identity.json` — strictly more than a
logged-in local ADMIN can read. With flash encryption off, those files are the key set. The OLED requester
side (OLED_Mode_FileBrowser.cpp:477/550) reaches `fsGetSendRequest` from a plain A-press with no admin check.

| # | Finding | Location |
|---|---------|----------|
| 6 | `blePushEvent` unbounded `snprintf` accumulator overflows a 256-byte stack buffer and notifies it over the air | Bluetooth.cpp:2210-2233 |

**What breaks:** Confirmed verbatim. `int pos = snprintf(eventJson, sizeof(eventJson), ...)` takes the
*would-be* length; two unconditional `pos += snprintf(eventJson + pos, sizeof(eventJson) - pos, ...)` follow,
where `256 - pos` is `size_t` and wraps to ~4.29e9 while the base pointer is already past the array. Then
`setValue((uint8_t*)eventJson, pos)` hands the BLE stack an out-of-range length. `cmd_bleevent` is registered
`requiresAdmin=false` and takes the raw argument tail. ~240 chars overflows; 2000 chars clobbers caller frames
in `cmd_exec_task` and leaks them to the connected peer. **Mitigation note:** the `bleScRequired()` gate at
:2214 early-returns — but `bleScRequired()` is `gSettings.bleRequireSecureChannel && secret.length() > 0`
(System_BleSecureChannel.cpp:184), so it is **false on a default device** and the overflow is reachable
out of the box.

| # | Finding | Location |
|---|---------|----------|
| 7 | `logout g2` is guest-allowed and re-homes the G2 lens identity to the super-admin owner | System_User.cpp:3384; System_Utils.cpp:4356, 5334 |

**What breaks:** `cmd_logout` is `requiresAdmin=false`; `commandAllowedForGuest` splits on the first space and
allows the entire line when the first token is `logout`. `logoutTransport` calls `g2PairedUserClear()` then
`bleStampPairedByIfBlank()`, and with `pairedByUser` blank the resolver falls through to
`getDeviceOwnerUsername()` — the first users.json entry, the superadmin. A view-only guest permanently
upgrades the whole G2 surface to super-admin with `FsRole::SUPER` filesystem access. Two-line fix.

| # | Finding | Location |
|---|---------|----------|
| 8 | Non-admin `sensorlog start <path>` performs arbitrary create/append/rename/delete as `FsRole::SYSTEM` | System_SensorLogging.cpp:2074, :1109 |

**What breaks:** Registration confirmed `requiresAdmin=false` (:2074, third field). The only path validation is
`filepath.charAt(0) != '/'`, and every FS call uses unscoped `VFS::systemAuth()` → `PERM_ALL`. A rank-1 user
runs `sensorlog rotations 0` then `sensorlog start /system/users/users.json 100` and the tick loop appends
rows into the live auth database, then deletes it at the rotation threshold — auth lockout, and the next
person at the console becomes superadmin via first-time setup. All boards. The autostart path (:2186) makes
it persistent with no AuthContext constructed at all.

| # | Finding | Location |
|---|---------|----------|
| 9 | The G2 lens executes CLI and reads the filesystem as `FsRole::SUPER` with no credential | G2_HijackCmd.cpp:177-184 |

**What breaks:** `tgRequireAuth` short-circuits `SOURCE_G2_GLASSES` to "pairing is auth" and only checks the
username string is non-empty; when empty, `bleResolveStampUsername` falls through to `getDeviceOwnerUsername()`.
`resolveRole` then returns `FsRole::SUPER` and `permsForRole` returns an unconditional `PERM_ALL` that ignores
the matched rule entirely. The lens Files page is *more* permissive than an authenticated web admin — it reads
`/system/certs/*.key` and other users' PBKDF2 hashes. "Pairing is auth" is deliberate; the silent escalation of
pairing to SUPER rather than ADMIN is not, and `AuthContext::scope` is already plumbed and unused.

| # | Finding | Location |
|---|---------|----------|
| 10 | `WebMirrorBuf::append/appendDirect` write the NUL terminator one byte past the allocation | WebServer_Utils.cpp:91, :140 |

**What breaks:** `init()` allocates exactly `cap` bytes (:35) and both trim loops are `while (len + need > cap)`
(:71, :121), so `len == cap` is reachable and `buf[cap] = '\0'` writes into the next PSRAM heap block's
header. `gWebMirror` is 8192 B and takes **every** broadcast line, so the exact-fit case is a matter of when —
and an attacker can steer it by choosing command output length. `assignFrom()` at :146 does the same
arithmetic correctly, which is what makes this an oversight rather than a design choice. Filed twice at two
severities; HIGH is correct.

| # | Finding | Location |
|---|---------|----------|
| 11 | The `@` / `remote:` wrapper defeats every redaction rule; raw credentials land in the web-readable feed | HardwareOne.cpp:498; System_Utils.cpp:1115-1141, :1185 |

**What breaks:** Confirmed at HardwareOne.cpp:498 — `String line = String(prefix) + redactCmdForAudit(cmd);`
then `gWebMirror.appendDirect(...)`. `redactCmdForAudit` matches with `cl.startsWith(r.prefix)` against a
table of bare verbs (`"login "`, `"wifiadd "`, `"useradd "`), so one leading `@` sidesteps all 18 rules.
`/api/cli/logs` admits any plain `user`. **This does not depend on bond mode:** the feed write is upstream of
the `#if ENABLE_ESPNOW && ENABLE_BONDED_MODE` branch (confirmed, guard at System_Utils.cpp:4605), so
`@login bob hunter2` is mirrored in cleartext even though the command returns "Bond mode not available".
The sibling echo at :4671 was already fixed to redact — the feed sink was simply missed. **Note:** the
*authorization-bypass* half of this cluster (peer executes as `kBondAdminUser`) is compiled out; see §2.

| # | Finding | Location |
|---|---------|----------|
| 12 | `handleSettingCommand()` ignores `writeSettingsJson()`'s result and reports success | System_Settings.cpp:2645, :2660, :2668, :2677 |

**What breaks:** All four type branches do `if (!gDeferWrites) writeSettingsJson();` with the bool discarded,
then unconditionally return the success string, broadcast "<key> set to <value>" and post
`SYSEVT_SETTING_CHANGED`. `writeSettingsJson` has four real failure exits, each of which logs "settings NOT
persisted" while the caller answers OK. The only unconditional item in the top 15: every one of ~276 settings,
every surface, no precondition. The operator sees `OK:`, the audit log records `-> OK`, the device reboots with
the old value — and it breaks the project's own uniform OK:/Error: contract at its busiest chokepoint.

| # | Finding | Location |
|---|---------|----------|
| 13 | `writeText()` discards `f.print()`'s byte count and returns `true` unconditionally | System_Utils.cpp:813-825 |

**What breaks:** Confirmed verbatim: `f.print(in); f.flush(); f.close(); return true;`. A full or failing
LittleFS produces a short/empty tmp file that `writeTextAtomic` then renames over the destination. The
credential store is the dominant caller — users.json goes through it from `createInitialAdminUser` and every
role/password mutation. The failure mode is not visible data loss: the device reboots into
`SETUP_REQUIRED`, where the wizard mints the next account with `role="superadmin"`, handing ownership to
whoever touches serial or the OLED next. **Scope correction:** `writeTextAtomic`'s rename-failure path is
already correct (:840-846 refuses the truncate-in-place fallback); the defect is only the unchecked print.

| # | Finding | Location |
|---|---------|----------|
| 14 | Mesh passphrase decrypted before device-key epoch selection, and the writer destroys the preservation blob | System_Settings.cpp:1379, :1386, :1390; System_ESPNow.cpp:1375 |

**What breaks:** `readSettingsJson` decrypts BLE peers and ESP-NOW meshes *before* `selectDeviceKeyEpoch`,
the function whose stated purpose is to run "before anything decrypts". `getSecret` is a bare `decryptString`
with no failure detection. And `espnowMeshesWriteJson` opens with `doc["espnow"]["meshes"].to<JsonArray>()`,
which clears the merge-read array, so `putSecretPreserving`'s prevBlob is always empty and its
"keep the previous stored value" branch is structurally unreachable for meshes. One bad decrypt empties the
passphrase in RAM and the next settings write commits `""` over the still-recoverable AES blob — permanent,
no attacker needed. The WiFi writer twenty lines away (:961-977) does it correctly with a comment naming this
exact hazard.

| # | Finding | Location |
|---|---------|----------|
| 15 | Fragment reassembly does not bind per-fragment authentication to the slot | System_ESPNow.cpp:5251-5290, :5315, :5595 |

**What breaks:** This defeats the `REQ_SESSION_ENC` gate the entire ESP-NOW security model rests on, for every
fragmented opcode. SESSION_FRAME unwrap happens per-fragment and sets a *local* verdict; the reassembler keys
slots on `(src_addr, msgId)` and stores nothing about auth state, session id or type; dispatch is handed the
verdict of whichever frame completed the slot. The V4 header is cleartext even on SESSION_FRAMEs and dedup
runs *after* reassembly. An in-range attacker sniffs `msgId`, injects a plaintext middle fragment with a valid
CRC16, and the genuine encrypted fragment is dropped as a duplicate. Same reach against `FILE_DATA` and
`CMD_RESP`. Fix is small: record `(isSessionEncrypted, isAuthenticated, sessionId, type)` on the entry at
allocation and drop the slot on mismatch.

### Immediately below the line

These are real, verified, and would be top-15 in a shorter list. They lost on reachability breadth or on
requiring an unusual precondition — not on confidence.

- **Automations scheduler cache use-after-free** (System_Automation.cpp:533-541) — `heap_caps_free` then
  pointer-nulled *before* the count, while loopTask on core 1 both reads and **writes** `gAutomationsCache[n]`
  in `rebuildAutomationsCache`. The file already contains the correct decision for its sibling buffer at
  :3695-3701 ("Deliberately never freed... would hand a live reader a dangling pointer"). 832 bytes; matching
  that comment is the whole fix.
- **Sixteen boot-path FATAL `while (1) delay(1000)` wedges** (HardwareOne.cpp:1339 + 15 others) — TWDT panic
  unset, only IDLE0/IDLE1 subscribed, RTC WDT disabled before app_main. The :1339 wedge fires before
  `oledEarlyInit()`, so the unit is indistinguishable from dead hardware on every power cycle. :1529 bricks
  the device over a failed ~1.5 KB automation-cache alloc inside an if/else that already knows how to skip.
- **Arduino `File::flush()`/`close()` are void** (vfs_api.cpp:294-305, :352-358) — ENOSPC is structurally
  invisible, which makes every "flash full?" check in the tree inert. This is the *root cause* of the entire
  write-integrity family. Practical constraint the finders missed: `components/arduino` is gitignored, so a
  patch there is a silent revert on vendor resync — the fix must be a checked-commit helper in System_VFS.
- **`annotateActivityDaily` stack corruption on BTC_TASK** (System_R1_Protocol.cpp:837-844) — same
  `off += snprintf` accumulator shape as item 6, into `char abuf[256]` in `ringDumpFrame`, triggered by an
  ordinary ring activity sync. **Widening the 48-byte reserve does not fix it** — `off` must be clamped.
- **`imgProbeWorker` frees its own stack, TCB and context while executing on them** (G2_Page_TestSuite.cpp:1049-1056)
  — reachable only on the static-task fallback at :1114, and masked on flash-touching probes by the companion
  PSRAM-stack assert. Deleting the fallback resolves both.
- **LC3 decode inline on BTC_TASK** (G2_Glasses.cpp:1880) — re-measured against the shipped ELF:
  `handleAudioNotify` 1728 B + `lc3_mdct_inverse` 3904 B ≈ 6.4 KB of an 8192 B stack, no end-of-stack
  watchpoint, ~50 packets/s for the whole recording, on the single task servicing both temples *and* the ring.
- **`resolvePendingUserCreationTimes` 8 KB truncation** (System_User.cpp:3216, :3317) — `readBytes(buf, 8191)`
  with no compare against `f.size()`, then `writeTextAtomic`. The truncation lands mid-object, so the committed
  file is **invalid JSON** and every account fails lookup — total auth lockout, not partial loss.
- **Unauthenticated SESSION_OPEN flood** (System_ESPNow_Handlers_Crypto.cpp:998-1015) — size check, PSRAM
  alloc and `submitDeferredToCmdExec` with no identity, pairing or rate check; each item costs a ~3 KB-stack
  Ed25519 verify on the single serializer for *every* command on the device.
- **`.svg` served inline as `image/svg+xml`** (WebServer_Server.cpp:4345, :4383) — no Content-Disposition, no
  X-Content-Type-Options; `.svg` absent from both `isImageFile()` and `hasSensitiveExtension()`. Complete
  upload → admin clicks View → same-origin POST to `/api/cli`.
- **No brute-force lockout on any non-web credential surface** (System_User.cpp:439-459) — worst case is BLE:
  Bluetooth.cpp:715 handles `login ` and returns at :772, *before* the `bleRequireAuth` gate at :793. Amplified
  by the gamepad pattern being a full network credential with a **256-candidate keyspace**, valid over BLE,
  ESP-NOW, MQTT and the web form.
- **`devices.json` hand-serialized with unescaped peer strings** (System_ESPNow.cpp:536-603, :7473-7492) —
  METADATA rows carry `flags = 0`, `metaGetSet` strips only *surrounding* quotes, and a failed
  `deserializeJson` on next boot zeroes `deviceCount` so the next save rewrites the file empty, taking every
  peer MAC, per-device AES key and meshId. Reachable by an owner typing a quote.
- **`[MEMSAMPLE]` DRAM total underflows** (System_MemoryMonitor.cpp:180) — `totalHeap` is already
  internal-only (~400 KB) and `totalPsram` is 8 MB, so the `size_t` subtraction wraps. Upgraded because this
  repo's active heap-offload and lazy-allocation projects read their before/after numbers off this telemetry.
- **`MapCore::loadMapFile` unloads before its permission check, on an ANON identity** (System_Maps.cpp:341-361;
  OLED_Mode_Map.cpp:1106) — not a guest issue: loopTask installs no identity scope, so `existsGuarded` always
  denies for *every* role, and Maps → Next Map reliably destroys global map state shared with the web page,
  the G2 lens and every `mapinfo` consumer.

---

## 2. Refuted — what did not survive

41 findings failed verification. Grouped by theme, with the code fact that killed each.

### Command execution, authorization & write integrity (T1)

| Finding | Killed by |
|---|---|
| `imagedelete` deletes any absolute path as SYSTEM | Board field says "all"; it is not. System_ImageManager.cpp:19 wraps the file in `#if ENABLE_CAMERA_SENSOR`, which is 1 only on XIAO Sense. The active build is `ARDUINO_UM_FEATHERS3_DEV`. `cmd_imagedelete` does not exist on this board. Real on XIAO Sense; severity and reach as claimed are not. |
| CPU-clock I2C drain guard covers only power-save; in-tree justification is wrong | **The central claim is backwards and the in-tree comment is right.** IDF's esp32s3 `rtc_clk.c:236` sets APB to a fixed 80 MHz on the PLL path — 80/160/240 MHz switches do not change the I2C source clock. Only `rtc_clk_cpu_freq_to_xtal()` (:413) reclocks APB, and that is exactly the path already routed through `setCpuFrequencyDrained()`. Leaving `applyPowerMode()`/`cmd_cpufreq` raw is a defensible design decision. |
| `remote:`/`@` defeats redaction — leg 2, unredacted broadcast | **Stale — already fixed.** System_Utils.cpp:4670-4672 builds `String safeCommand = redactCmdForAudit(actualCommand);` and both the `snprintf` and the `broadcastOutput` use it, under a SECURITY comment describing verbatim the bug being reported as live. Only two legs survive (see top-15 item 11). |
| I2C error logging is unbounded per-error | **Partially refuted on the two branches it leads with.** `recordError()` sets `health.degraded = true` at 3 consecutive NACK (:813) / TIMEOUT (:842) errors and `executeTransaction` then hard-skips the device with a 30 s retry window — roughly one log line per 30 s, not a per-poll flood. Survives only for BUS_ERROR (:855-880) and BUFFER_OVERFLOW (:884-886), which never set `degraded`. |
| `srSnipDeinit` force-deletes the writer mid-write, orphaning the FS mutex | `ENABLE_ESP_SR 0` (System_BuildConfig.h:293). The finding's own board field says so and it is still filed HIGH. Same for the EdgeImpulse free-while-running entry (`ENABLE_EDGE_IMPULSE 0`, :296) and the XIAO camera SCCB port-1 collision (`ENABLE_CAMERA_SENSOR 0`). |
| Nine sensor create helpers call `eTaskGetState()` on a dangling handle | Use-after-free is real; **the consequence is not.** `eTaskGetState` only *reads* `pxTCB->xStateListItem.pvContainer`; the freed block is still mapped DRAM and cannot fault. The observable failure is a garbage state that is neither `eDeleted` nor `eInvalid`, so the handle stays non-null and the sensor silently refuses to restart for the rest of the boot. Functional bug, not a crash. |

### ESP-NOW mesh & protocol (T2)

| Finding | Killed by |
|---|---|
| USER_SYNC → planted "bond-admin" escalates admin→super-admin | Both `kBondAdminUser` short-circuits are inside `#if ENABLE_BONDED_MODE` (System_User.cpp:284-293, :362-367) and that flag is **0** (System_BuildConfig.h:320). A planted account falls through to the ordinary lookup, and `doUserSyncWork` hard-codes `newUser["role"] = "user"`. The attacker also already had to hold a valid ADMIN credential, and such an attacker can run `useradd` directly. Residual (missing `isValidPublicUsername`) is MEDIUM and already filed three more times. |
| Arduino re-applies `WIFI_PS_MIN_MODEM`, dropping async ESP-NOW RX | Mechanism correct, **consequence follows on neither branch.** Unassociated: `initEspNow` leaves a hidden soft-AP beaconing on the ESP-NOW channel, which keeps the radio awake regardless of the STA's PS setting. Associated: System_WiFi.cpp:1086-1094 re-asserts `WIFI_PS_NONE` inside the `WL_CONNECTED` branch, strictly after STA_START. A sibling entry states the correct mechanism and rates it LOW. |
| Bond token in PSRAM is the super-admin credential; `v4_handle_cmd` stamps `kBondAdminUser` | The `@BOND:` branch is inside `#if ENABLE_BONDED_MODE` (:5755-5824) and the `#else` arm at :5817-5827 **explicitly rejects** any `@BOND:` payload precisely because it would be RCE without the validators. `isBondSessionTokenValid()` is a `return false` stub (:900). Nothing on the device will accept a bond token. The PSRAM-residency half of both findings is verified and survives. |
| "Bonded command from %s (session token)" leak sink | `BROADCAST_PRINTF` at :5781 is inside `#if ENABLE_BONDED_MODE`. Cannot execute. The finding's *other* sink — `"[ESP-NOW] Remote command from %s (user=%s): %s"` at :5838 — is live and does hold. Keep the finding, drop the bond half. |
| 47 CLI report builders size against 1024 while the buffer is 4096 | **The safe direction.** A 1024 bound on a 4096 buffer cannot overflow under any input; there is no hazard being "prevented". The actual defect is that long reports are silently truncated at 1024 while 3 KB goes unused. Reclassify as NOTE. |

### Web server & HTTP API (T3)

| Finding | Killed by |
|---|---|
| `/api/bond/exec` and `/api/bond/cli/batch` give any non-guest super-admin execution on the peer | Entire body of WebPage_Bond.cpp is inside `#if ENABLE_WEB_BOND` (line 6). `CUSTOM_ENABLE_WEB_BOND 0` (System_BuildConfig.h:89) → `ENABLE_WEB_BOND` derives to 0. `ENABLE_BONDED_MODE` is also 0. Neither handler is compiled or registered. |
| `/api/bond/fs/get\|list\|stat` reads arbitrary peer files with no admin gate (×2) | Same compile-out. The duplicate's board field reads "all (… && ENABLE_BONDED_MODE && ENABLE_WEB_BOND)" — labelling "all" a conjunction whose second and third terms are both 0. The peer-side SYSTEM-identity observation is accurate and survives as top-15 item 5. |
| Bond role-swap split-brain guard is structurally inert | `handleBondRole` is inside `#if ENABLE_WEB_BOND` (=0). Not compiled. The underlying observation — `executeCommand`'s remote branch returns true on *enqueue*, not on peer CMD_RESP — is correct and worth keeping as an ESP-NOW note. |
| `handleMqttStatus` hands raw String interior pointers to blocking chunked writes (×2) | Both assert board = "all builds with ENABLE_HTTP_SERVER && ENABLE_MQTT **(FeatherS3 included)**". That parenthetical is a direct factual error: `#define ENABLE_MQTT 0` (System_BuildConfig.h:104) and the derived rule forces `ENABLE_WEB_MQTT 0`. WebPage_MQTT.cpp is not compiled. |
| `/api/cli/batch` returns unredacted output — bounding claim "no command emits a `sid` field" | `cmd_session_list` → `buildAllSessionsJson` emits `session["sid"] = s.sid` for every live session at WebServer_Server.cpp:696 — the full 32-hex value. The mechanism half is right; the bound is wrong. Correct bound is a sibling's: `sessionlist` is `requiresAdmin=true`, so exposure is admin-to-admin. Two entries in this 9-way cluster reason from contradictory facts about the same code. |
| 121 of 124 `httpd_register_uri_handler()` unchecked; "~43 slots left, the count is a floor" | 124 raw grep hits are a **ceiling** for any single build. 29 are in TUs compiled out on FeatherS3 (Bond 14, LLM 12, MQTT 2, Games 1). Real count ≤95 against 160 slots — ~65 slots of headroom. The unchecked-return defect is genuine; the urgency is not. |
| Migration restore is "the attacker-reachable way to plant an AuthBypass superadmin… no owner mistake required" | `handleRestore`'s first gate is `gFirstTimeSetupState != SETUP_IN_PROGRESS \|\| !gAcceptingRestore → 403` (:634). `gAcceptingRestore` is set true at exactly one site — the owner selecting "Import from Backup" on the OLED/serial menu — and `applyStagedMigrationRestore()` has exactly one caller, the on-device confirm. Three owner actions on a factory-reset device. Entirely an owner mistake. The missing destination allowlist and users.json schema validation are real. |

### Bluetooth, G2 glasses & R1 ring (T4)

| Finding | Killed by |
|---|---|
| `deinitG2Client()`/`closeg2 full` teardown race | **Cites the wrong lines and misses an explicit quiesce handshake.** `templeReset` is at :8372-8397, not :8316-8331, and it *contains* the quiesce the finding says is missing: it clears `t.connected`/`writeChar`/`notifyChar` first, then takes `writeMutex` with a 2000 ms timeout to exclude any sender mid-write, deletes the client, gives, and only then deletes the semaphore — with a comment stating that reasoning verbatim. `sendEnvelopeEx` gates on `!t.connected` *before* touching the mutex, so the claimed 1500 ms block cannot form. `deinitG2Client` also calls `g2Disconnect()` first, killing the rxBuf window. Residual is LOW. |
| `bleCentralTxInit()` is a non-atomic lazy singleton reachable from two cores | Check-then-act is real, **reachability premise is false.** All four external take sites are on send paths that first require a live temple or ring link, and establishing either requires `initG2Client`/`g2RingInit` — both of which call `bleCentralTxInit()` on cmd_exec_task before any send path exists. No path in which a take precedes an init. Downgrade LOW → NOTE. |
| `gRing` GATT pointers cleared field-by-field → null-`this` `writeValue()` | The stated failure mode does not follow. `ringClearGattPointers` deliberately does **not** delete the client (comment at :116-122 explains keeping the pointer with `clientStale=true` to avoid leaking the ~10-14 KB GATT cache), so this is a stale read, not a use-after-free. The null-`this` outcome also requires the compiler to *re-load* a non-volatile member between the recheck and the call, which it is free not to do. And the clear writes `connected=false` before `writeChar=nullptr`, working against the reader. CONFIRMED is unsupportable. |

### Filesystem, settings & secret storage (T5)

| Finding | Killed by |
|---|---|
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` puts every TLS secret in plaintext PSRAM | Mechanism real, **harm does not hold.** The headline is that a bus-probing attacker recovers the HTTPS identity — but that private key is already plaintext on **unencrypted flash** (`CONFIG_SECURE_FLASH_ENC_ENABLED` unset; System_WiFi.cpp:1543 writes the raw PEM). Anyone who can probe QSPI PSRAM can dump SPI flash with less effort. Zero incremental exposure for the key. Plus `httpsEnabled` defaults false and `certgen` is superadmin-gated, so a default device has no TLS material at all. The doc correction survives. |
| Every user's PBKDF2 hash is readable by any plain Admin — "offline Admin → SuperAdmin escalation" | Disclosure holds; **the escalation does not.** The PBKDF2 salt is `getDeviceEncryptionKey()`, derived from eFuse MAC + `esp_flash_read_unique_chip_id`. That flash UID is read at exactly two sites tree-wide and is never surfaced by any command, endpoint or log line (`deviceKeyFingerprint()` is a truncated one-way SHA-256). A remote Admin cannot reconstruct the salt and cannot mount **any** offline crack. Reconstruction needs physical possession, at which point flash is unencrypted anyway. Survives as a shared-password oracle + policy break. |
| Device-key epoch selection is blind to the credential database — failure chain (1) | Chain (2) is real and confirmed. Chain (1) is not a defect of `selectDeviceKeyEpoch` at all: when `nBlobs == 0` it adopts `cand[0] = deriveDeviceKeyFromIds(chipId, flashUid)`, byte-identical to what the lazy path would derive anyway. The selector changes nothing. The "transient failure" premise is also weak — `esp_flash_read_unique_chip_id` is deterministic per part. |
| `setSetting()` is void — OK: after failed persist (**the named trigger**) | Conclusion correct and duplicates the HIGH; **the trigger is fabricated.** There is no 5120-byte doc budget. `PSRAM_JSON_DOC` is an elastic ArduinoJson 7 document with a PSRAM allocator, so `doc.overflowed()` means the *allocator* failed. The "5120 bytes" figures are stale comments from an ArduinoJson 6 `StaticJsonDocument`. A settings.json past 5 KB does not trigger this; PSRAM exhaustion does. |
| Stack-overflow detection is canary-only — **the prescribed fix** | "The watchpoint is mutually exclusive with the canary, so it is a build-flavour decision" is **false.** `FREERTOS_CHECK_STACKOVERFLOW` is a choice block (Kconfig:51-79); `FREERTOS_WATCHPOINT_END_OF_STACK` is an independent `bool` at :373 with no `depends on`. Both can be on simultaneously — the help text describes it as an addition. The fix is cheaper than claimed. Two sibling entries get this right; only this one is wrong. |

### Users, auth, automation & MQTT (T6)

| Finding | Killed by |
|---|---|
| Automation `createdBy` "system" is a total `authorizeCommand` bypass (HIGH) | The exploit as written — "a plain `user` POSTs name=system/automations.json" — is refuted. The rule row at System_Filesystem.cpp:1350 has `userPerms` literally **0**, and `permsForRole` returns that for USER and `& PERM_READ` for GUEST. No user, guest or admin can write it through any guarded path. Only `FsRole::SUPER` or SOURCE_INTERNAL can — and both already bypass `authorizeCommand` by definition. This is privilege **persistence** / audit evasion, not escalation. The MEDIUM duplicate states it correctly. |
| MQTT topic/payload treated as NUL-terminated — unbounded `strlen` (HIGH) | (a) **Not compiled:** CMakeLists.txt:15 sets `HW_CFG_ENABLE_MQTT 0`, and the `if(HW_CFG_ENABLE_MQTT GREATER 0)` block at :376 is the only thing appending System_MQTT.cpp to the sources. (b) Crash claim overstated: the over-read is real (verified against esp-mqtt `mqtt_client.c:1124-1128`) but stays inside a large contiguous mapped DRAM region — a garbage String and an oversized allocation, not LoadProhibited. |
| MQTT bridge reports `ok:true` unconditionally (HIGH) | Code claim confirmed, **HIGH unjustifiable**: a Home-Assistant response-correctness bug with no security, memory-safety or availability impact, in a file that is not compiled. A second finder filed the identical defect at MEDIUM. |
| `isValidUser` user-enumeration timing oracle (HIGH) | Structurally correct but **HIGH is the outlier**: two other finders filed the same divergence at LOW (explicitly noting the delta was never measured) and MEDIUM. Impact is enumerating a handful of account names on a personal device, and it is strictly less useful than the unthrottled-guessing finding beside it — knowing a username buys nothing when you can spray passwords without limit. |
| `executeConditionalCommand()` scans past the end of the command string (HIGH) | Bug is real (`for (size_t i = 3; i < cmdLen - 5; i++)` at :3380 with no `cmdLen>=6` guard, and the ELSE twin at :3400). But `cmd_conditional` is genuinely unregistered, so the only live entry is the automation runner — and both its registry rows are `requiresAdmin=true`, while hand-editing automations.json needs `FsRole::SUPER`. An admin-authored malformed automation crashing the device is a robustness defect, not attacker-reachable. |
| No brute-force lockout — **sub-claim** that lockout state sits behind `#if ENABLE_HTTP_SERVER` at :449-459 | Partial correction, not a refutation. The main claim holds. But the cited lines are the guard around `recordLoginAttempt` — the **audit-ring writer** — not the lockout counter, which is not in this file at all. The conclusion happens to be right because the counter lives in a web TU, but the evidence points at the wrong code. |

### I2C, sensors & OLED (T7)

| Finding | Killed by |
|---|---|
| Location-context bar overflow — **board rationale** (both copies) | Both state the wrong trigger and wrong gate. They claim `ENABLE_GPS_SENSOR` on "I2C_FEATURE_LEVEL 3 – FeatherS3 primary". Shipping config is **level 4** with `CUSTOM_ENABLE_GPS 0`, so the entire block setting `hasGPSFix` from the local GPS cache is inside `#if ENABLE_GPS_SENSOR` at :1067-1082 — compiled out. The bug survives (top-15 adjacent) but requires the ESP-NOW remote-GPS fallback at :1084-1096, not a local GPS module. |
| FM-radio poll nests two manager transactions — **CRITICAL rating and blast radius** | Self-deadlock mechanism holds. The CRITICAL rests on effects that cannot occur: the whole file is `#if ENABLE_FM_RADIO` (:19) and that is 0. There is no `fmRadioTask`, so "the bus is unavailable ~80% of the time" and "starves the gamepad poll" are impossible. The finding also gates itself on `ENABLE_FMRADIO_SENSOR`, **a macro that does not exist anywhere in the tree**. |
| Four OLED paths use the bus-0 helper — **sub-claim (b)**, "Wire1 clocked to 400 kHz, blocking bus-0 devices" | Core bug real and live. (b) does not hold: `executeTransaction` restores the clock via `clockStackPop` + `setBusClock` *before* releasing the mutex (System_I2C_Manager.h:377-380, :425-426), and no other bus-0 device can transact while it is held. No lasting misconfiguration. Survives: the mutex hold, the unsynchronised multi-transaction write against bus 1, and the phantom 0x3D device in `i2chealth`. |
| `shouldBlockForDisplayAuth()` returns false for the whole OLED boot window | **Self-refuting as filed.** The finder's own `why` concedes the only reachable modes (OLED_ANIMATION, OLED_LOGO) have no action handlers and that no escalation could be constructed; confidence is PLAUSIBLE. A structural note about a duplicated predicate, not a security finding. |

### Build config, boot, docs & LLM (T8)

| Finding | Killed by |
|---|---|
| "Nothing can recover a hung task: TWDT off, 14 boot-path FATAL handlers" (HardwareOne.cpp:1312) | **Not a separate finding and every cited line is stale.** :1312 is a `crashRecordEmitEarly()` comment; :1490 is the automation gate's `if`; :1505/:1516 are in the cmd-queue block but not on the wedge statements. `grep -n 'while (1) delay'` returns exactly **16** sites — precisely the site list of the other HIGH entry at :1339. Same defect, worse line accuracy, and it miscounts as 14. Fold into :1339. |
| vfs_api flush/close — **sub-claim** that a full FS "is reachable within minutes of boot" on sr_8mb | Mechanism holds, urgency does not. `partitions_sr_8mb.csv` is selected only when `ENABLE_ESP_SR=1`, which is 0. Worse, a sibling finding proves that config **cannot even link**: the layout declares factory 0x4E0000 = 5,111,808 B against a current binary of 5,237,744 B. Shipping layout is `partitions_no_sr_16mb.csv` with littlefs = 10,604 KB. |
| XIAO base-board block unreachable — **the prescribed fix** | The fix does not work. System_BuildConfig.h:176 is an **unconditional** `#define XIAO_ESP32S3_SENSE_ENABLED 1` with no `#ifndef`, and every consuming gate tests `defined(...)`, not the value. So deleting both CMake `-D`s still selects the Sense branch, and "zeroing line 176" changes nothing because `defined(X)` is true for `#define X 0`. Only deleting line 176 *and* both `-D`s reaches the base block. Conclusion stands; remedy is broken. |
| Both LLM findings: "loopTask / core 1" as one of the racing tasks | **There is no loopTask in this build.** `# CONFIG_AUTOSTART_ARDUINO is not set` (sdkconfig:603), and cores/esp32/main.cpp:25 wraps both loopTask and Arduino's app_main in that flag, so the file compiles to nothing. main/hardwareone-idf.cpp:26 supplies its own app_main, so setup()/loop() and all OLED code run on the IDF `main` task: **core 0**, priority 1 — the same core as cmd_exec_task. A NOTE in this same theme already establishes this and contradicts both entries. The race survives (httpd is `tskNO_AFFINITY`); the task/core map is wrong and would misdirect anyone reasoning about which pairs can interleave. |
| cmd_exec priority inversion — **the "CONFIRMED" starvation consequence** | Structure verified. Consequence is not confirmed: `espnow_task`'s loop body ends in an unconditional `vTaskDelay(pdMS_TO_TICKS(10))` (:9398), so it blocks every pass and cmd_exec is never starved in the FreeRTOS sense. Whether `processMeshHeartbeats` consumes the window under load is unmeasured — the finding itself asks for a `perftop` run. Confidence should be PLAUSIBLE. |

---

## 3. Severity corrections

### Downgraded because it is not compiled on FeatherS3

The single largest source of inflation. `ENABLE_BONDED_MODE 0`, `ENABLE_WEB_BOND 0`, `ENABLE_MQTT 0` /
`HW_CFG_ENABLE_MQTT 0`, `ENABLE_ONDEVICE_LLM 0`, `ENABLE_ESP_SR 0`, `ENABLE_CAMERA_SENSOR 0`, and
`CUSTOM_ENABLE_{APDS,THERMAL,FM_RADIO,PRESENCE,RTC,GPS} 0`.

| Finding | From → To | Note |
|---|---|---|
| MQTT mesh-routing branch skips `authorizeCommand` (all six filings) | HIGH → **LOW (latent)** | Verified correct as code — :483-516 calls `cmd_espnow_roomcmd`/`tagcmd`/`remote` directly and returns before `submitAndExecuteSync`; all three are `requiresAdmin=true`. Compiles on no board. **Re-raise to HIGH the moment `ENABLE_MQTT` flips.** |
| MQTT `updateExternalSensor` strlen over-read | HIGH → LOW | Real OOB read; file not compiled; fault claim overstated. |
| MQTT bridge `ok:true` for every failure | HIGH → LOW | Response-correctness only, dormant file. |
| POST `/api/llm/load` and `/unload` skip the admin gate the CLI enforces | MEDIUM → LOW | `ENABLE_ONDEVICE_LLM 0`; WebPage_LLM.cpp not compiled. Gate asymmetry vs System_LLM.cpp:3040-3041 is real — **fix before re-enabling.** |
| FM-radio poll self-deadlock | CRITICAL → **LOW (latent)** | `#if ENABLE_FM_RADIO` at :19, flag is 0. |
| `apdsColorPoll()` unbounded data-ready spin hangs cmd_exec_task | HIGH → LOW (latent) | `CUSTOM_ENABLE_APDS 0`. Genuinely the only such loop left; still fix it. |
| Thermal and FM sensors can never restart after any stop | HIGH → LOW (latent) | Verified: :228/:281 are the only nullptr writes tree-wide, so the `if (handle == nullptr)` wrapper makes recovery unreachable. Both files gated out. |
| STHS34PF80 wraps bus-aware helpers in a bus-0 transaction | HIGH → LOW (latent) | `CUSTOM_ENABLE_PRESENCE 0`. In-file comment at :586-590 already documents the hybrid split — a fixer has a head start. |
| DS3231 TZ save/restore aliases `getenv("TZ")` into `setenv("TZ")` | HIGH → LOW (latent) | `CUSTOM_ENABLE_RTC 0`. The automation `nextFire()` consequence is the sharp end — fix before re-enabling. |
| Base (non-Sense) XIAO board block unreachable | HIGH → LOW | Finding's own board field concedes "FeatherS3 unaffected"; every consumer is nested inside `defined(ARDUINO_XIAO_ESP32S3_DEV)`. Latent trap; the misleading comment at System_BuildConfig.h:166-176 is worth fixing. |
| ESP-SR models still built (~2.98 MB srmodels.bin) | NOTE → **LOW** | Understated as pure build cost: the same sdkconfig block is what makes the partition-picker at CMakeLists.txt:129-148 hard to reason about — the second reader-hazard in that file family. |

### Downgraded on reachability or overstated consequence

| Finding | From → To | Note |
|---|---|---|
| `executeCommand()` returns true for every dispatched command | **CRITICAL → MEDIUM** | The bool means "dispatched", consistent with the documented OUTPUT CONTRACT at :4439-4467 where the `Error:`/`OK:` prefix is the result channel. Only two callers read it; no security decision does. Residual: the unknown-command branch (:4811) and JSON error payloads being audited OK. |
| `imgProbeWorker` frees its own stack/TCB while running | **CRITICAL → HIGH** | Two qualifiers the CRITICAL entry omits but a duplicate states: only on the static-task fallback at :1114, and masked on flash-touching probes by the PSRAM-stack assert. Deleting the fallback resolves both. |
| `mic_record` busy-spins at priority 5 on core 1 | **CRITICAL → HIGH** | Confirmed spin with only `taskYIELD()` and unreachable exit conditions. Core 0 keeps running, so `micrecstop` still recovers it, and TWDT is non-panic. |
| Always-on log caps overcommit the LittleFS partition | HIGH → MEDIUM | Not the shipping layout. Active build is 16 MB with littlefs 10,604 KB against ~2.7 MB of caps (~26%). |
| `CLIMode sActiveMode` written by two tasks | HIGH → MEDIUM | Two-writer claim confirmed, but the null-deref needs a mode defining `onTick` — only the wizard does. Confined to first-boot setup, not "every board, always". |
| AuthBypass sentinel can become a real promotable account | HIGH → MEDIUM | Both halves confirmed in code, but the FTS half needs the owner to type "AuthBypass" at their own prompt and the USER_SYNC half needs an already-compromised admin plus a follow-up `userpromote`. Missing one-line validator with a nasty payoff. |
| Cleartext passwords written into PSRAM on every credential path | HIGH → MEDIUM | Mechanism confirmed and its correction of the prior audit ("they stay internal") is right and worth keeping — `ps_alloc` calls `heap_caps_malloc(MALLOC_CAP_SPIRAM)` directly, bypassing ALWAYSINTERNAL, and `ExecReq` has no destructor. But recovery requires physically probing the quad-SPI bus. Standing-rule violation, not remotely exploitable. |
| Newline in a command line forges audit records | HIGH → MEDIUM | Reachability confirmed (web CLI urlDecodes, so `%0A` survives; `logCommandExecution` does not scrub, though `sanitizeField` does on the event-log path). Gain is forensic-log integrity only; the actor already holds an account. |
| HTTPS silently downgrades to plain HTTP | HIGH → MEDIUM | **"Silently" is wrong** — :5278-5281 emits both a `logSystemEvent` and a `broadcastOutput` warning, and the ssl_start branch does the same. The downgrade and the unchecked certgen writes are real. |
| Reassembly bounds-checks index against attacker's `fragCount` | HIGH → MEDIUM | (a) and (b) are **not memory-safety events**: `have[254]` reads `buffer[233]` and `have[21]=true` writes `buffer[0]` — both inside the same object. Only (c) is a genuine defect: a 0-length phantom fragment completes the message with a hole, delivering never-cleared prior-message residue. |
| `reassembledSize` uses the completing frame's `payloadLen` | HIGH → MEDIUM | Same containment. The real defect is the honest-path one buried at the end: any legitimate multi-fragment message completing **out of index order** is delivered with the wrong length and trailing residue — normal radio reordering, no attacker. |
| `espnow roomcmd`/`tagcmd` count every peer as "Sent" | HIGH → MEDIUM | Discarded return confirmed. Honest-reporting defect, not security or availability. |
| No free-space reserve — a peer may push a declared 4 MB file | HIGH → MEDIUM | Gap is real, but the sharpest argument ("LittleFS is smaller than a single permitted transfer") is specific to the 8 MB layouts. On FeatherS3 the attack needs ~19 minutes at the measured ~9 KB/s. |
| `writeIdentityFile` accepts any non-zero serialize length | HIGH → MEDIUM | Code fact exact; needs LittleFS to fill *mid-write* of a ~300-byte doc during one of a handful of lifetime writes. Total impact, very narrow trigger. |
| BROADCAST_AUTH replay suppresses backup-master failover | HIGH → MEDIUM | Mechanism holds but **names the wrong frame**: the dedicated master→backup heartbeat at :8701 goes through `v4_send_payload_smart` (AEAD, replay-protected). The replayable one is the *periodic* mesh heartbeat at :8673. Works, via a different frame, and only against a deployment with `meshBackupEnabled` plus a configured master MAC. |
| Four web status endpoints run handlers inline on the httpd task | HIGH → MEDIUM | Confirmed mechanically, but the finding's own analysis bounds it: every writer self-caps and NUL-terminates, so the outcome is garbled status text, not memory unsafety. One of four cited sites is compiled out. |
| Plaintext HTTP is the default → passive cookie sniff | HIGH → MEDIUM | Verified, but `ENABLE_HTTPS` is 1, certs are user-installable, and the finding's own body states every endpoint is correctly gated. An accepted deployment posture already recorded in PRE_1_0_HARDENING_AUDIT §2.3. Actionable half is the silent-fallback visibility suggestion. |
| SSE hold loop spins forever past 24.86 days uptime | HIGH → MEDIUM | Mechanism exact. Needs >24.855 days **and** a socket write failure **and** queued notices simultaneously. Consequence when it fires is severe and silent — do not dismiss, but it sits below the always-on findings. |
| `deinitG2Client()` teardown race | HIGH → **LOW** | Central mechanism refuted. Residual: the give-then-delete pair at :8395-8396, and `bleConnectShutdown`'s bare `vTaskDelete` at :9044-9048 — which the comment at :9041-9043 explicitly accepts as a deinit-time tradeoff. |
| `gRing` GATT pointers cleared field-by-field | MEDIUM/CONFIRMED → **LOW/PLAUSIBLE** | Stale read, not use-after-free; null-`this` depends on a compiler reload the standard does not require. |
| `appendLineWithCap()` rotation write-integrity (all four reports) | HIGH → MEDIUM | Confirmed verbatim, but the destructive path needs free space inside a **~325 KB band on a 10,604 KB partition**: `resolveOverflowPath` latches log writes to SD below 100 KB free, and the rotation transient is ~425 KB. FS must be ~96-99% full. |
| `readTextLimited()` reserves the full byte cap before reading | HIGH → MEDIUM | Verified. No corruption, no security consequence — worst case is latency/fragmentation *after* `reserve()` already failed. The unconditional 12,304-byte internal-DRAM demand on every G2 text-file open is the part that deserves attention. |
| `saveUserSettings()` truncates the live credential file as its rename fallback | HIGH → MEDIUM | Code confirmed exactly. Only runs when `renameGuarded` fails — realistically ENOSPC on the metadata commit, the same near-full precondition. High impact, low probability. |
| Device-key epoch selection blind to the credential database | HIGH → MEDIUM | Chain (1) refuted; chain (2) requires a prior re-key **plus** a secret re-save during that boot. |
| Automation `createdBy` "system" bypass | HIGH → MEDIUM | Persistence, not escalation. |
| `executeConditionalCommand` size_t underflow | HIGH → MEDIUM | Only live caller is the admin-gated automation runner. |
| `isValidUser` timing oracle | HIGH → LOW | Two other finders filed it at LOW and MEDIUM; delta never measured. |
| Every user's PBKDF2 hash readable by any plain Admin | HIGH → MEDIUM | Escalation refuted (salt unreconstructable remotely). Shared-password oracle + policy break; fix by setting `adminPerms` back to 0. |
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` | HIGH → **LOW** | Retain **only** for the HOT_PATH_HEAP_AUDIT_MECHANICS §4 doc correction, which is genuinely wrong for mbedTLS. |
| Four OLED bus-0 transaction sites | HIGH → MEDIUM | Live and worth fixing, but after removing the refuted clock claim the damage is garbled frames on a teardown race, a bounded mutex hold, and a phantom 0x3D in `i2chealth`. Cosmetic and diagnostic. |
| `oledDirtyUntilMs` monotonic max vs absolute millis() compare | MEDIUM → LOW | Needs a timed popup armed in the seconds before a 49.7-day wrap. Same class as the sibling rollover cluster it overlaps — they should carry the same severity. |
| cmd_exec priority inversion | MEDIUM/CONFIRMED → MEDIUM/**PLAUSIBLE** | Severity right, confidence label wrong. |

### Upgraded

| Finding | From → To | Why |
|---|---|---|
| Unauthenticated SESSION_OPEN flood onto the cmd_exec queue | **MEDIUM → HIGH** | `v4hSessionOpen` (:998-1015) does a size check, a PSRAM alloc and `submitDeferredToCmdExec` with **no identity, pairing or rate check** — every gate lives in the deferred worker. Each item costs a ~3 KB-stack Ed25519 verify on the single serializer for every command on the device. A credential-free radio-range flood stalls the whole CLI, not just ESP-NOW. Broader blast radius than several HIGHs above it. |
| `/api/files/view` serves `.svg` inline as `image/svg+xml` | **MEDIUM → HIGH** | Verified in full: `.svg` in the isImage test at :4345, `image/svg+xml` at :4383, no Content-Disposition, no X-Content-Type-Options anywhere in the server, and `.svg` genuinely absent from both `isImageFile()` and `hasSensitiveExtension()`. Complete stored-XSS → `/api/cli` chain, structurally identical to the ESP-NOW twin this theme rates HIGH. |
| `WebMirrorBuf::append/appendDirect` one-past-end write | MEDIUM → **HIGH** | Filed twice at two severities; HIGH is correct. 8192 B buffer taking every broadcast line — the exact-fit case is a matter of when. |
| `[MEMSAMPLE]` DRAM total underflows to ~4.19 M KB | MEDIUM → **HIGH** | Arithmetic confirmed; the code comment rests on a false premise. Matters more than MEDIUM because this repo's active heap-offload and lazy-allocation projects read before/after numbers off internal-DRAM telemetry. |
| Maps → Next/Previous Map unloads before the permission check | MEDIUM → **HIGH** | The guest framing buries the real bug: loopTask installs no identity scope, so `currentAuthContext()` is ANON and `existsGuarded` denies for **every** role. Destroys the live map and fails to reload it, on the current board. |
| PRE_1_0_HARDENING_AUDIT declares the MQTT backlog item resolved | NOTE → **MEDIUM** | The doc's cited :529 evidence is in code the mesh branch never reaches, and "The MQTT backlog item is resolved" is the newest written statement on it — it will close a live gap. One caveat the finding overstates: the mesh branch forwards the caller's own credentials to the peer, so the defect is "a non-admin MQTT client invokes the espnowremote family without the local admin gate", not full escalation on the peer. |
| `resolvePendingUserCreationTimes` 8 KB truncation | HIGH → HIGH (**impact restated**) | Severity stands, description is wrong: truncation lands mid-object, so the committed file is **invalid JSON** and every account fails lookup — total auth lockout, not "accounts past the boundary are deleted". Impact understated; likelihood overstated (needs ~32-40 accounts plus a pending null `createdAt`). |
| G2_Health's eight history rings hold 9,376 B of internal DRAM | MEDIUM → MEDIUM (**confidence raised**) | Independently verified in the link map: `.bss._ZL4sBat` @ 0x3fcaa128 and `.bss._ZL4sHrv` @ 0x3fcaaee4, each 0x494 = 1172 B in internal DRAM, ×8 = 9,376 B, and `ENABLE_R1_HEALTH` is 1 on this build. Treat as fully measured, not estimated. |

---

## 4. Duplicates

505 findings collapse to roughly **215 distinct defects**. The CRITICAL/HIGH tier is where duplication is
worst. Highest-multiplicity clusters first.

**9×** `/api/cli/batch` returns unredacted output while `/api/cli` redacts — all at WebServer_Server.cpp:5093-5096.
Severities range MEDIUM/LOW. Two of the nine reason from **contradictory facts about the same code** (see §2).

**8×** `imgProbeWorker` frees its own stack + TCB before `vTaskDelete(NULL)` — all G2_Page_TestSuite.cpp:1049/1052,
split CRITICAL(1)/HIGH(7).

**6×** ESP-NOW `FS_LIST`/`FS_STAT`/`FS_GET` under SYSTEM identity — entries [10] CRITICAL, [24], [57], [60],
[68], [100]. **[100] alone adds a genuinely distinct second half**: the OLED/web *requester* side has no
admin/role gate that the CLI `espnowbrowse` copy enforces. Keep [100] as the surviving copy.

**6×** MQTT mesh-routing branch skips `authorizeCommand` — three severities across six filings.

**5×** each: `writeText()`/`writeTextAtomic()` unchecked write return (indices 43, 78, 85, 139, 175);
`certgen` writes both PEMs unchecked (44, 79, 140, 161, 176); `espnowroomcmd`/`espnowtagcmd` missing from the
kRules redaction table (2, 12, 22, 97, 107); ESP-NOW key material in PSRAM ([4], [27], [62], [63], [28]) —
**two distinct objects at most**, `gSessions` and `gEspNow`, five write-ups; `g2_img_probe` PSRAM task-stack
fallback (G2_Page_TestSuite.cpp:1108/1109); reassembly `have[]` indexed by wire `fragCount` ([17](a), [23],
[45], [58], [93]) — **[58] is the only one that correctly labels it a benign in-struct over-read**;
guest-allowlisted `GET /api/gps/tracks?live=…` (WebPage_Maps.cpp:109-147).

**4×** `remote:`/`@` bypasses `authorizeCommand` (7, 17, 26, 100) — *plus* a separate 4× cluster on the same
wrapper defeating redaction (20, 5, 24, 88), all four of which share the now-stale "unredacted broadcast" leg.
**4×** USER_SYNC appends users.json without `isValidPublicUsername` ([11], [13], [52], [102]) — one missing
call at System_ESPNow.cpp:3658. **4×** `executeCommand` returns true / success inferred from an "Error" prefix
(83, 84, 174, 177). **4×** `appendLineWithCap` write-integrity — all pointing at the same two unchecked writes
(`println` at :1828, `w.write` at :1889), plus `copied` counting bytes *read* not written and an unconditional
`VFS::remove(dest)` at :1899. **4×** G2 credential submit-echo logging (G2_HijackCmd.cpp:100/:106/:138/:144).
**Nuance none of the four states:** the three `DEBUG_ALWAYS` sites are all *failure* paths; the normal success
path is only :138, which is `%.40s`-truncated and needs `DEBUG_G2`. **4×** dead `/login/setsession`
unauthenticated session mint (WebServer_Server.cpp:3643).

**3×** each: full session SID broadcast (all CRITICAL, WebServer_Server.cpp:376/:322); `devices.json`
hand-rolled JSON ([37], [46], [98] — same file, same line 585; **[40], the abort-mid-object corruption mode,
is distinct and should be kept**); post-rekey replay-window reset ([7], [66], tail of [103]);
`espnowApplyChannel` commits before the unchecked radio calls ([31], [84], [87], [34], [89]/[36] — one
function, :978-990); TIME_SYNC inert ([8], [49], [72]); inbound remote command echoed unredacted ([15], [61],
[101] — same BROADCAST_PRINTF at :5838); POST `/api/cli` uncapped `ps_alloc` with no NULL check (:3194);
unauthenticated frames enqueue heavy crypto ([21], [59], [69]); boot corrupt-JSON check naming the dead path
`/settings.json` (System_Filesystem.cpp:174/175, three filings); stack-overflow detection posture
(sdkconfig:1977/:2008 — one of the three gets the Kconfig relationship wrong); MQTT payload password leak
(:441); missing brute-force lockout on non-web transports; `isValidUser` timing oracle; `tgRequireAuth`
else-branch comment false about MQTT/ESP-NOW; truncate-in-place persist fallbacks (`writeSettingsJson` :1093,
direct-write :1106, `saveUserSettings` :3131); MQTT mesh-route unescaped positional splicing (:487/:493/:494 —
one site, three framings); Arduino File flush/close (vfs_api.cpp:304/:336/:352 — **the :336 entry has the best
mechanism write-up and :304 the best consequence list**).

**2×** (one line each): `WIFI_PS_NONE` vs Arduino's cached MIN_MODEM ([35], [41], [90], [95] — **[95] gets the
mechanism right and effectively refutes the other three**); confirm-mode `yes` has no requester identity
([8], [16], [19] — **[19] is the superset**); AuthBypass reserved name in first-time setup; `events.log` tees
raw command text; Arduino loop/setup on core 0 not APP_CORE; runtime-settable I2C pins accept MSPI/strapping
GPIOs; `initBus()`/`performBusRecovery()` discard `TwoWire::begin()`'s bool; `i2creset` recovers only bus 0;
XIAO camera SCCB port collision; `applyPowerMode()`/`cmd_cpufreq` skip the drain guard; MEMSAMPLE underflow;
`CommandArgs::value()` unanchored substring match; `enqueueChunk()` false ISR-safety claim (3×); debug flags
dump raw command lines; automation sub-command logging unredacted; `setupWiFi()` stalls boot; audit-log
rotation has no boot orphan recovery; `bootStateInit()`'s `nvs_flash_erase()` destroys BLE bonds and WiFi
config; boot monotonic→wall-clock offset latched once; `BATTERY_ADC_PIN` decorative; `cmd_factoryreset`
discards timer returns; `wifiscan` blacks out the mesh; stack-watermark reporting covers a hardcoded subset;
SESSION_OPEN replay tears down a live session ([0]/[64], identical); auto-mode channel derived from the live
radio ([33]/[88]); `captureEspNowFrame`'s 976 B stack frame; SENSOR_BROADCAST accepted with flags=0;
`saveEspNowDevices`/`saveMeshPeers` write-integrity; dead `totalTime` expression; `decodeBasicAuth`
off-by-one; httpd left at `tskNO_AFFINITY`; unsynchronized SSE rings; WebMirrorBuf one-past-end;
`handleMqttStatus` String lifetime; `/api/cli/batch` hardcodes `{"ok":true}`; web login leaks
username + password_len; `handleCameraStream` `while(true)`; `httpd_resp_send_chunk` return discarded;
`ip_bans.json` truncate-rewrite; LC3 decode on BTC_TASK; R1 `systemTime` sent with tz=0; "ISR-safe"
mislabelling in Bluetooth.cpp; OLED renders raw inbound BLE command line; "no automated test" ;
per-UI-action task spawning; `pageSwapInit`'s 4× stack-myth comment; `g2micwav`/`g2micrec` inline FS write on
BTC_TASK; ramflush ESP-NOW field mismatch; flash cache-disable window; `-Os`/`-O2` defaults conflict;
ramflush treats link state as user intent; settings `OK:`-on-failed-persist; OLED gamepad pattern as network
credential; automations cache freed under a live reader; sub-second automation rewrites; `startMQTT` discards
returns; `publishDiscoveryConfig` snprintf accumulation; `mqttstatus` unsigned underflow;
`System_AuthIdentity.h` stale comment; automation `createdBy` bypass; OLED_Mode_Map `contextBuf` overflow;
OLED_Mode_Map PSRAM memcpy under `taskENTER_CRITICAL` (**keep the :1200 copy — it closes the cache-off
question via the ETS_IPC_ISR_INUM level-4 argument**); OLED_Mode_Map "8KB stack in PSRAM" comment (**both
correctly handled the bytes-vs-words footgun**); sensor-logging unchecked `f.write()` (**keep the HIGH**);
sensor-logging start-gate free-space check; DS3231 TZ aliasing (**merge keeping the MEDIUM copy's board
note**); 16 boot-path FATAL wedges (**keep :1339 — line-exact and correctly counts 16**); LLM load/unload
locking (System_LLM.cpp:249 and :1008 — `gLLM.mutex` is taken only in `llmEnsureWorker`); docs/BOARD_SWITCHING.md
(:194 and :291 — one stale-document item).

**Notable disagreements between duplicates.** The two BLE-scan-duty-cycle entries disagree on arithmetic and
**the G2_Ring.cpp:975 one is wrong** — it says "at the BLE 0.625 ms unit that is a 62.5 ms interval",
converting backwards. `BLEScan::setInterval/setWindow` take **milliseconds** and divide by 0.625, so
`setInterval(100)/setWindow(99)` really is 99 ms on per 100 ms. Conclusion survives; keep the :1006 entry.
Likewise `MAX_SESSIONS=2` (NOTE) and `findFreeSessionIndex` always returning -1 (LOW) are two halves of one
session-eviction defect and should be merged.

**Cluster worth treating as one ticket, not five.** The sensorlog CRITICAL (:1109), the autostart HIGH
(:2186), the unchecked-write HIGH (:909), the rotation-path truncation MEDIUM (:928) and the start-gate
MEDIUM (:1178) are all consequences of **one design decision**: `sensorlog` is `requiresAdmin=false` and every
filesystem call in it uses unscoped `VFS::systemAuth()`. One fix — scoped `systemAuth` or caller-identity
passthrough — closes all five.

Similarly, "the main loop runs under a sticky SYSTEM identity" (main/hardwareone-idf.cpp:45) and "recon map
correction: loopTask does not exist" (:26) both rest on `CONFIG_AUTOSTART_ARDUINO` being off and correct the
same family of stale comments. Report as one item: *who owns the main loop, and with what identity.*

---

## 5. Per-theme health

**T1 — Command execution, authorization & write integrity.** Real but heavily over-counted: 51 CRITICAL/HIGH
entries collapse to roughly **26** distinct defects. The genuine core is one pathology repeated everywhere —
`authorizeCommand`/`redactCmdForAudit` are applied to a line that is not what actually executes, and
filesystem/hardware writes are treated as infallible. Both classes are confirmed and both are exploitable
today on FeatherS3. Against that, ~14 of the 51 do not survive: four are compiled out on the current board
(`imagedelete` is inside `#if ENABLE_CAMERA_SENSOR` despite its board field saying "all"), one has a central
claim that is **factually backwards about the ESP32-S3 clock tree**, one describes a broadcast leak the tree
already fixed, one over-scopes an I2C log storm the degraded-skip path bounds, and several overstate severity
for read-only-UAF, log-injection or physical-probe threats. Not a rubber stamp.

**T2 — ESP-NOW mesh & protocol.** Healthier than the raw refutation count suggests: of 34 CRITICAL/HIGH
entries opened, 5 fail on a specific code fact and 6 more are severity-inflated; the remaining 23 hold, several
with unusually good tracing. **The dominant failure mode was missing `#if ENABLE_BONDED_MODE`** — hard-0 at
System_BuildConfig.h:320, and it silently deletes the bond-token credential path, the bond-admin privilege
short-circuits, and one of two credential-leak sinks, which is exactly the escalation half of three separate
HIGHs. Second failure mode was calling in-struct index arithmetic "OOB read/write" and rating it HIGH. The
theme's genuine weak spot is coherent and unflattering: **every ESP-NOW authentication decision is made
per-frame and then re-applied to state that outlives the frame** — the reassembly slot inherits the last
fragment's verdict, SESSION_OPEN/SESSION_REKEY carry no freshness, BROADCAST_AUTH has no counter, and the
post-rekey window zeroes the replay bitmap while leaving the previous RX key installed. Layered on top is a
persistence tier that cannot fail safely. 104 findings → ~45 distinct issues.

**T3 — Web server & HTTP API.** Not healthy but not as bad as 24 crit/high suggests: the real count is closer
to **14**, because 78 findings collapse to ~40 unique defects (one issue filed nine times, another five). Seven
crit/high entries do not survive contact with the board configuration — WebPage_Bond.cpp is entirely
`#if ENABLE_WEB_BOND` and **two of those entries label their board field "all"**. The same check kills the two
MQTT String-lifetime MEDIUMs, which explicitly and wrongly claim "(FeatherS3 included)". ~17% refutation on
crit/high. What survives is serious and concentrated in **auth plumbing rather than handlers**: the session-token
log leak, the pre-auth base64 off-by-one, the ESP-NOW→innerHTML chain, an 8-slot lockout table that evicts
exactly the row an attacker wants gone, guest-readable `/api/automations` returning raw `wifiadd <ssid> <psk>`,
and the `gDeferWrites` strand that silently disables settings persistence device-wide for a boot. Credit where
due: nobody misread a `*_STACK_WORDS` constant, and one entry correctly caught that httpd `stack_size` is
BYTES not WORDS — **but that catch is buried as an "otherSites" aside and deferred to another track, which is
the most consequential thing this theme currently has no owner for.**

**T4 — Bluetooth, G2 glasses & R1 ring.** Badly inflated by duplication rather than bad findings: 61 entries
→ ~30 distinct issues, and the 25 CRITICAL/HIGH collapse to **9** — 13 of the 25 are the same two
`imgProbeWorker` defects filed eight and five times. On substance the finders were mostly right, with several
claims independently re-confirmed against the shipped ELF and link map. Nobody made the `*_STACK_WORDS` 4×
error, and **no crit/high finding is refuted by board gating** — `ENABLE_BLUETOOTH` and `ENABLE_G2_GLASSES` are
both unconditional `1`. The one genuine casualty is the `deinitG2Client` teardown race, which cites line
numbers that do not match the function and misses an explicit take-before-delete handshake documented in a
comment directly above the code it quotes. Most trustworthy theme in the set.

**T5 — Filesystem, settings & secret storage.** All 14 CRITICAL/HIGH hold mechanically — none could be killed
outright, which is itself suspicious, and the reason is severity inflation rather than fabrication. **Five of
the fourteen carry a stated consequence that does not survive contact with the code**, and six deserve a
downgrade once preconditions are counted. Strip that out and the theme is really **five** distinct issues: one
genuine CRITICAL path-canonicalization bypass, one permanent-secret-loss bug in the mesh read/write ordering,
one unconditional silent-persist-failure contract violation, one `appendLineWithCap` family (filed four times),
and one cluster of admin-vs-superadmin permission-boundary leaks. The finders were **rigorous about mechanism
and consistently sloppy about reachability** — asserting "permanent lockout" or "offline escalation" without
checking whether the attacker can obtain the salt, whether the file exists on a default device, or how wide
the free-space window is. Two thirds of the entries are sdkconfig posture notes and doc corrections correctly
filed as NOTE/LOW; they should not compete for fix time with the three above.

**T6 — Users, auth, automation & MQTT.** Two structural problems dominate. (1) Massive duplication: 50 entries
→ ~20 distinct defects, with the MQTT `authorizeCommand` bypass alone filed **six times at three severities**.
(2) Severity inflation from ignoring the build config: `ENABLE_MQTT 0` **and** `HW_CFG_ENABLE_MQTT 0` mean
System_MQTT.cpp is never added to the sources — **not compiled on any board today**. Sixteen of the 50 entries
are MQTT, four of the five MQTT HIGHs do not flag this, and every one should be latent-not-live. Strip the
dormant block and the duplicates and about **six real live defects** remain, three of them serious and all
three compiled on FeatherS3. The non-MQTT findings held up well — the `logout g2` chain, the automations cache
UAF, the users.json truncation, the missing lockout, the gamepad-pattern credential and the
`revokeUserSessions exceptSid` bug all verified against actual code. The one non-MQTT HIGH whose exploit
narrative is flatly wrong is the automation `createdBy` entry.

**T7 — I2C, sensors & OLED.** Mechanically unusually solid: not one of the 11 crit/high findings is
contradicted by the code. Guards, mutex types and call chains are exactly as reported. That is a suspicious
result and it is stated as such — but the reason is careful finders, not skipped verification. **The real
correction is elsewhere: five of the 11 are on code that is not in the shipping binary.** The build is
`ARDUINO_UM_FEATHERS3_DEV` / `I2C_FEATURE_LEVEL 4` with `CUSTOM_ENABLE_{APDS,THERMAL,FM_RADIO,PRESENCE,RTC,GPS}`
all 0, and those drivers are file-level `#if`-gated; their CRITICAL/HIGH labels are not defensible today. What
survives as live is narrower than the counts suggest: the sensorlog privilege cluster (all boards), one stack
overflow in the OLED map context bar, one destructive map-load ordering bug, and the bus-0 OLED helper. Two
findings also misstate their own board reachability in ways that matter, and six of 37 entries are exact
duplicate pairs.

**T8 — Build config, boot, docs & LLM.** Better than the raw count suggests: 7 HIGH entries collapse to **4**
independent issues (three are the same Arduino `File` flush/close hole; two are the same 16-site boot wedge).
All 4 survivors hold on their central claim, and the mechanisms are unusually checkable — void return types, a
grep-exact wedge count, a prefix-anchored rule table, two unconditional `-D`s. **The real signal is at the
sub-claim layer:** one entry's line numbers are entirely stale, one entry's urgency rests on a partition layout
that **cannot currently be built** (declared factory 5,111,808 B vs a 5,237,744 B binary), one entry's
prescribed fix does not work because the gate tests `defined()` rather than the macro's value, and both LLM
entries name a `loopTask` that `CONFIG_AUTOSTART_ARDUINO=off` deletes from the build. The docs sub-theme is the
strongest part of the set — MEMORY_LAYOUT, BOARD_SWITCHING, PRE_1_0_HARDENING and HOT_PATH_HEAP_AUDIT_MECHANICS
all check out as genuinely wrong against code that was read, and the ALWAYSINTERNAL reversal is worth acting on
because it has been blocking a ranked fix on a premise IDF's `heap_caps.c:117-121` contradicts outright.

---

## 6. Gaps — what has not been looked at

### The highest-value gap: a live bytes-vs-words error in HTTP server sizing

**This repo's signature footgun is sitting in live config and no finding owns it.**
WebServer_Server.cpp:5210 sets `sslConfig.httpd.stack_size = 11059` with the comment *"Stack arg is in
WORDS … 11059 words = 44 KB"*, and :5303 sets `config.stack_size = 7680` claiming *"7680 words = 30 KB …
measured peak ~18 KB leaves ~12 KB headroom (40%)"*. Both are wrong in the direction this project already
knows about: `esp_http_server`'s `osal.h` passes `config.stack_size` straight to
`xTaskCreatePinnedToCoreWithCaps`, and IDF's Xtensa port defines `portSTACK_TYPE uint8_t`, so the argument is
**BYTES**. The real stacks are ~10.8 KB and 7.5 KB. Either the httpd task is running at 40% of a claimed
budget with a "measured" peak larger than the whole allocation — i.e. the measurement is fiction — or someone
trusts the stated 12 KB headroom and adds a stack-hungry handler. Two findings flag the same mislabelling in
cosmetic log strings and a stale comment, but **not in this live sizing decision**. Latent only because
`httpsEnabled` defaults false.

### Second-highest: the settings staging file lives outside the protected tree

`writeSettingsJson` uses `const char* tmp = "/settings.tmp"` (System_Settings.cpp:1057) — at the **LittleFS
root**. `lookupRule()` matches no rule for that path, so it falls to the catch-all `PERM_ALL`. A plain
user-role account therefore holds full permissions on the file that transiently contains the entire serialized
settings document, and can **delete it mid-write** — which makes `renameGuarded` fail, which drives
`writeSettingsJson` into the truncate-in-place fallback at :1098 that another finding already identifies as
destructive. Nobody filed the two halves together and no finding notices the tmp file's rule placement at all.
Same question is unexamined for `/system/debug.json`'s staging name.

### Storage and persistence

- **LittleFS mount-failure policy is completely unaudited.** Nothing asks what `initFilesystem()` does when
  `LittleFS.begin()` fails — whether it passes `formatOnFail=true` and silently reformats a partition holding
  users.json, settings.json and all capture data. That is the single highest-consequence storage decision in
  the firmware and it is not in the list.
- **NVS is entirely absent** (raised independently by two themes). The nvs partition is 24 KB across all four
  CSVs, esp_wifi keeps its own config there, WiFi PSKs are stored there in plaintext per project history, and
  `nvs_flash_init`'s erase-and-retry-on-corrupt path is a data-loss branch nobody looked at. No coverage of
  NVS-full handling, wear, or the `esp_err` returns from `nvs_set`/`commit` — even though the theme's headline
  issue is precisely "persistence errors are swallowed".
- **Flash write amplification / endurance.** Several findings compute per-erase stall time in milliseconds;
  none counts erases per day. The 4 Hz automation tick, sensor logging, and the per-setting full-file rewrite
  of settings.json each rewrite whole blocks. On ~100k-cycle NOR flash this is a wear question nobody posed.
- **Backup/restore integrity.** No finding audits the restore side — whether a restored bundle re-validates
  paths through the rule table, whether it can write `/system/users/users.json`, or how it interacts with the
  device-key fingerprint. Adjacent to two HIGHs.
- **OTA / firmware-update path: zero coverage**, despite two findings turning on partition geometry and
  app-size headroom.

### Concurrency and scheduling

- **The cmd_exec queue itself.** `gCmdExecQ` depth, `submitCommandAsync` failure paths on a full queue, and
  whether a blocking handler can starve every other surface. Since cmd_exec_task is the single serialization
  point for *every* transport and runs at `TASK_PRIORITY_LOW` on core 0, its saturation behaviour is the
  biggest untested assumption in the audit.
- **The actual `gSettings` concurrency race** — only the doc describing it is corrected. No finding examines
  which fields are torn-read, whether String-typed settings (unprotected pointer swaps) are the exposed ones,
  or whether any lock exists.
- **CLIMode single-slot as a denial of service.** `cliRequestConfirm` returns false if any mode is active, so
  one user leaving a confirm prompt open blocks `filedelete`/`userdelete`/`factoryreset` for every other user
  on every transport, with no timeout and no way to clear it.
- **`SensorCacheGuard` acquisition failures.** The `if (g.held)` pattern silently drops the update on timeout;
  nothing asks what happens tree-wide when it is false — report, retry, or publish stale data as fresh.
- **`I2CDeviceManager::performBusRecovery`, device degradation and auto-disable thresholds** — the machinery
  every transaction finding depends on for its failure semantics, including the `recordError`-outside-the-mutex
  dance that is explicitly commented as deadlock-sensitive.

### Untrusted input surfaces with no coverage

- **ESP-NOW RX ingress:** `onEspNowDataReceived` → the 8-slot RX ring → the drain. Ring-overflow behaviour,
  what is dropped, and the fact that `onEspNowRawRecv` calls `captureEspNowFrame` from the WiFi driver's
  receive callback context.
- **The `espnow_tx` clerk** (`System_ESPNow_Tx.h`) has **no findings at all** — job queue depth, payload buffer
  lifetime across an async job, and what a caller's stack- or PSRAM-resident payload is doing when
  `sendAeadSync` times out at 2000 ms.
- **`FILE_*` receive-path filename handling:** the wire `filename[64]` arrives with no guaranteed NUL and is
  used to build `/espnow/received/<MAC>/<name>`. Nothing checks `..`, absolute paths, or embedded slashes.
- **`meshFingerprint` is a pure, unkeyed function of the mesh label** and is the first-line RX filter. Nobody
  looked at collision behaviour or at an outsider computing a target's fingerprint from a guessable label
  without the passphrase.
- **Inbound BLE frame parsing from both peers.** `g2ParseEnvelope` and the downstream sid/flag handlers against
  a hostile envelope; `ringDumpFrame`/`ringNotifyThunk` decoding attacker bytes on BTC_TASK — discussed only for
  cache coherency, never bounds. **This is the one remote-input surface in T4 and it has zero coverage.**
- **GAP/advertisement parsing:** `classifyG2Name` and `BLEAdvertisedDevice::parseAdvertisement` process
  attacker-controlled advertising payloads; examined only as "name matching is not authentication".
- **`/api/files/upload` multipart parsing** — the write half of the `.svg` stored-XSS chain, and the one file
  endpoint that writes attacker-supplied bytes of arbitrary length. Appears only as an "otherSites" line.
- **`evaluateCondition`** — the automation condition parser with its +17 resolver variables — gets no scrutiny
  as a parsing surface, even though the sibling parser in the same file had a `size_t` underflow.
- **The LLM `.bin` model loader** parses a user-uploadable file (info-block, tokenizer table, vocab/quant
  headers) with no finding on bounds or length validation. If `ENABLE_ONDEVICE_LLM` is ever flipped, that is
  where a real exploit lives — and both existing LLM findings are about load/unload locking instead.

### Auth, session and web plumbing

- **The web session layer itself is untouched:** `makeSessToken` entropy, MAX_SESSIONS eviction,
  session-fixation on `setSession`, cookie attributes, idle expiry — even though several findings depend on
  session behaviour for their impact story.
- **No CSRF / Origin / Referer analysis anywhere.** SameSite=Strict is the only thing between a hostile page
  and `POST /api/cli`, and nothing verifies that property holds on the **Basic-Auth fallback path**, which
  carries no cookie and no origin check at all.
- **No CSP header anywhere in the server**, and the `esc()` JS-string-escaper-used-as-HTML-escaper class
  (~15 sites) is referenced as background by two findings but never filed as its own issue — despite being the
  substrate that makes both confirmed XSS chains exploitable.
- **The BLE Secure Channel is cited as a mitigation by at least three findings but never audited** — nonce
  construction/reuse, replay windows, key derivation, session teardown on reconnect. If it is the recommended
  fix, its own correctness is load-bearing and unexamined.
- **Per-connection BLE session-token lifetime**: whether a token survives disconnect, and what happens when
  Bluedroid reuses a `conn_id` after a peer drops. A carry-over there would undercut every "defaults are safe"
  claim in T4.
- **Role-mutation authorization.** `userRoleRank` exists and the comment claims demote/ban/grant limits are
  "enforced in the user-mutation handlers" — nothing verifies that, nor what happens when the last superadmin
  self-demotes and the `isSuperAdminUser` first-user fallback silently re-grants.
- **`redactCmdForAudit`'s rule set is never audited**, despite three findings asserting credentials land
  unredacted in command-audit.log. The redaction allowlist is the actual control and nobody checked it.
- **Hand-rolled users.json scanning is systemic, not a one-off.** `getDeviceOwnerUsername` and
  `isSuperAdminUser` both walk the file with `indexOf`/`substring` — the same fragility the truncation finding
  flags, on the two functions that decide who is the owner and who is super.
- **Only 1 of the 25 guest-allowlist entries was traced end-to-end.** `/api/settings`, `/api/user/settings`,
  `/api/devices`, `/api/sessions`, `/api/espnow/metadata`, `/api/waypoints` and `/api/icon` are unexamined.
- **`logAuthAttempt(true, ...)` fires unconditionally at the end of every `executeCommand`** — nobody asked
  whether that makes successful_login.log useless as a login record.
- **`WebServer_MigrationTool.cpp:75-79` sets `Access-Control-Allow-Origin: *` with `Allow-Headers` including
  `Authorization` on `/api/backup`** — an auth-bearing endpoint. Only `/api/ping`'s wildcard CORS was examined.
- **`submitAndExecuteSync` returning the always-true bool is filed only against MQTT.** WebServer_Server.cpp:1143
  consumes the same bool on a surface that **is** compiled; whether the web/CLI JSON `ok` field is honest was
  never checked.

### Board reality vs audit effort

- **The audit is inverted relative to what ships.** Every deeply-audited I2C driver (APDS, thermal, FM,
  presence, RTC, ToF, IMU) is compiled out, while `i2csensor_seesaw.cpp` appears only as a victim of other
  findings and **`i2csensor_max17048.cpp` — the FeatherS3 fuel gauge, the board's only always-on I2C device
  besides the OLED — appears nowhere at all.**
- **Stale board comments caused two findings to misstate applicability and were never flagged themselves:**
  System_BuildConfig.h:132 says `CUSTOM_ENABLE_GPS` is "re-enabled 2026-07-03" and :139 says the same for
  `CUSTOM_ENABLE_RTC`, yet **both are set to 0**. A comment audit of the board block is cheap and would stop
  the next reader making the same error.
- **The plain-HTTP `httpd_config_t` never sets `max_open_sockets`**, so it runs at the IDF default of 7 with
  `lru_purge_enable=true` — while the HTTPS branch deliberately sets 8 with a long comment explaining that a
  browser's 6 parallel connections plus 1 persistent SSE socket needs 8 or LRU purges a live connection. **The
  config the device actually ships on has the exact problem that comment documents.**
- **AUTOSTART is in a theme name and has zero coverage:** `automationAutoStart` vs `automationEnabled` vs
  `gAutomationSchedulerRunning` semantics, boot ordering of `initAutomationSystem` relative to filesystem/NTP
  readiness, the ramflush resume overlay's interaction with autostart flags, and the cmdKey-fallback trap
  already recorded in AUTOSTART_NAMING_UNIFICATION_PLAN.md.
- **`System_CrashRecord.cpp/.h` are new and uncommitted** and the only finding about them is a CMake comment
  typo. Nobody examined the RTC record's integrity — magic/CRC validation, what survives a power cycle versus
  an EN reset, or whether `crashRecordSetPhase`'s phase byte can be misattributed to the wrong boot. **That
  matters directly to the boot-wedge finding, whose whole argument is "the wedge leaves no trail".**
- **`docs/COMMAND_REFERENCE.md` is new, untracked, and gets no accuracy check** against the actual registry —
  while four other docs in that theme were audited and **all four were found stale**.
- **Nothing examines time/RTC/timezone handling**, which project history flags as a live footgun (G2
  quarter-hours vs R1 raw minutes) and which feeds the dated-capture and per-day CSV rollover code that is
  currently uncommitted.

### Smaller, still open

- No audit of the other `annotate*` parsers in System_R1_Protocol.cpp (`annotateGenericDaily`,
  `annotateHealthPoint`, `annotateSleep`, `annotateDeviceInfo`) for the same `off += snprintf` overrun shape
  that makes `annotateActivityDaily` corrupt the stack — same 256-byte caller buffer.
- No verification that ESP-NOW frames from a **non-registered source MAC** actually reach the recv callback on
  this IDF version. **Several attack narratives depend on it and none checked.**
- `V4_REASM_MAX` is 2 — attacker exhaustion is covered, honest-path starvation is not: two peers streaming
  fragmented replies concurrently already saturate the table, and the loser gets a silent 5 s GC eviction.
- The 20-slot `esp_now` peer table and `addEspNowPeerWithEncryption` failure paths (table full, channel-0
  registration) are not reviewed anywhere.
- The discover→confirm pairing state machine is gated only by the mesh **group** key that every member holds;
  nobody assessed whether any member can drive an arbitrary peer's accept screen or exhaust the pairing state.
- No coverage of session-revocation timing — whether `enqueueTargetedRevokeForSessionIdx` actually closes an
  in-flight SSE stream.
- Per-link BLE MTU negotiation is untouched, despite a recently-diagnosed Bluedroid MTU bug whose patch lives
  in a **gitignored** `components/arduino` (a clean checkout silently reverts it). Nothing checks a peer
  negotiating a small MTU mid-transfer or how `setMTU` interacts with `sendPbFragmented`'s fragment sizing.
- `gRingTxQ`'s overflow behaviour, `coalesceKey` semantics and the correctness of its shift-and-insert under
  the critical section are never checked.
- The G2 receive-side reassembly state machine (`t.rxActive`/`rxSeq`/`rxHave`) is examined only from the
  seq-allocation side — nothing on interleaved envelopes from the two arms, or what `totFrags>1` does to state
  in code that admits it does not handle multi-fragment reassembly.
- `BLE_Peers`' auto-reconnect backoff is examined only for RF duty cycle; its state-machine correctness —
  unbounded retry, interaction with a concurrent user-initiated connect, and whether a failed reconnect leaves
  `gConnectTaskActive` latched (the 2 s give-up at G2_Glasses.cpp:9184-9187 **explicitly leaves the flag set**)
  — is uncovered.
- The OLED map's PSRAM budget as a *system interaction*: `initAsyncMapRenderer` takes 9 KB plus a tile-cache
  pool, on a quad-PSRAM board where LLM context auto-fit is already documented to collapse under pressure. The
  partial-allocation LOW treats it as a local leak, not contention.
- `HAL_Display.cpp`'s frame-push path is cited repeatedly as "the correct pattern" but never audited — the
  15 ms mutex timeout, the silent frame drop on contention, and the `pollPaused(oledBus)` early return are all
  load-bearing for the OLED findings.
- Both OLED_Mode_Map NOTE findings quote the render-task comment for its false "in PSRAM" claim but neither
  noticed the same comment block **also** says "No core affinity" while the call two lines down is
  `xTaskCreatePinnedToCore(..., APP_CORE)`. Two false statements in one comment, one flagged.
- MQTT broker-side security is entirely unexamined: how `mqttUser`/`mqttPassword` are stored, what
  `mqttTLSMode` actually verifies, and whether the command topic is world-writable on a typical broker — the
  precondition every MQTT finding assumes without stating.
