# RECON — Command-entry surface map & auth-gate matrix

Repo: `/Users/morgan/esp/hardwareone-idf` @ `f5aea65` (v0.99.3), working tree dirty.
Scope: every path by which a command string reaches `executeCommand()` (or an equivalent
handler invocation), plus where the auth gate sits on each.
Covers OVERNIGHT_AUDIT_BACKLOG task **B0**.

All findings below were read in source, not grepped. Line numbers are from the working tree.

---

## 0. The single chokepoint

Everything that is *supposed* to be gated funnels through one function pair:

| Symbol | File:line |
|---|---|
| `bool executeCommand(AuthContext&, const char* cmd, char* out, size_t)` | `components/hardwareone/System_Utils.cpp:4443` |
| `static bool authorizeCommand(const AuthContext&, const String& line, char*, size_t)` | `components/hardwareone/System_Utils.cpp:4300` |
| the call | `components/hardwareone/System_Utils.cpp:4481` |

```cpp
// System_Utils.cpp:4480
  // Centralized authorization (admin-required and future policies)
  if (!authorizeCommand(ctx, command, out, outSize)) {
    return false;
  }
```

`authorizeCommand` applies, in order (System_Utils.cpp:4300–4368):

1. **INTERNAL bypass** — `if (ctx.transport == SOURCE_INTERNAL && ctx.user == "system") return true;` (:4303)
2. **empty-identity reject** — `if (ctx.user.length() == 0 && ctx.transport != SOURCE_INTERNAL)` → deny unless the command is literally `login` (:4311)
3. **guest gate** — `if (ctx.user.length() > 0 && isGuestUser(ctx.user) && !commandAllowedForGuest(line))` → deny (:4323). `commandAllowedForGuest` (:4289) allows only `login` / `logout`.
4. **super gate** — `if (commandRequiresSuperAdmin(line) && !hasSuperAdminPrivilege(ctx))` → deny (:4339)
5. **admin gate** — `if (commandRequiresAdmin(line) && !hasAdminPrivilege(ctx))` → deny (:4353)

Both `commandRequires*` resolve through `findCommand()` (`System_Command.cpp:110`).
**`findCommand` returns `nullptr` for an unrecognised line, and `commandRequiresAdmin`
then returns `false` (System_Utils.cpp:3244–3253).** That is the root of gaps G1 and G3 below.

### Queue / submission helpers (all land on `cmd_exec_task`)

| Helper | File:line | Notes |
|---|---|---|
| `submitAndExecuteSync(const Command&, String&)` | `System_Utils.cpp:4764` | 2 s enqueue / 60 s exec timeout; falls back to a *direct* `executeCommand` when `gCmdExecQ == nullptr` (early boot, :4769) |
| `submitCommandAsync(const Command&, cb, void*)` | `System_Utils.cpp:4878` | fire-and-forget |
| `submitDeferredToCmdExec(fn, arg)` | `System_Utils.cpp:4925` | **bypasses the whole CLI pipeline** — no `executeCommand`, no auth. Used for BLE-SC handshake + ESP-NOW Ed25519 crypto only |
| `execCommandUnified(const CommandContext&, const String&)` | `System_Utils.cpp:4945` | thin wrapper over submitSync |
| `runUnifiedSystemCommand(const String&)` | `System_Utils.cpp:4957` | hard-codes `SOURCE_INTERNAL` / `user="system"` → **auth bypass by design** |
| `executeUnifiedWebCommand(req, ctx, cmd, out)` | `System_Utils.cpp:4980` | web helper, carries caller's ctx |
| `executeCommandThroughRegistry(const String&)` | `System_Command.cpp:191` | **calls `found->handler()` directly — NO AUTH GATE AT ALL** (:256, :263) |
| `dispatchCommand(const String&)` | `System_Utils.cpp:3266` | same, `entry->handler(argsInput)` at :3271 — **no auth**; currently no callers |
| `cmd_exec_task` worker | `HardwareOne.cpp:703` (dequeue) → `HardwareOne.cpp:742` (`executeCommand`) | deferred-fn fast path at `HardwareOne.cpp:710` skips executeCommand entirely |

---

## 1. Role tiers — definitions

Ranks are integer constants in **`components/hardwareone/System_User.h:98–103`**:

```cpp
constexpr int kRoleRankGuest      = 0;
constexpr int kRoleRankUser       = 1;
constexpr int kRoleRankAdmin      = 2;
constexpr int kRoleRankSuperAdmin = 3;
constexpr int kRoleRankNoGrant    = -1;   // sentinel for delete/ban
```

Role *names* (`"guest"|"user"|"admin"|"superadmin"`) are stored per-account in
`/system/users/users.json`; the integers are never persisted.

| Tier | Predicate | File:line | Semantics |
|---|---|---|---|
| Guest | `isGuestUser(who)` | `System_User.cpp:1245` | `userAccountRank(who) == kRoleRankGuest`. CLI surface reduced to `login`/`logout`; FS reads masked to `PERM_READ`; web restricted to a GET allowlist |
| User | default | `System_User.cpp:424` `userRoleRank` — unrecognised names collapse to `kRoleRankUser` | anything not admin-flagged |
| Admin | `isAdminUser(who)` | `System_User.cpp:278` | `role=="admin" \|\| role=="superadmin"`; **fallback**: first user in users.json with *no* `role` field is admin (:344). `kBondAdminUser` is admin while a live bond session exists (:286) |
| Super Admin | `isSuperAdminUser(who)` | `System_User.cpp:358` | `role=="superadmin"`; **fallback**: if *no* account is explicitly superadmin, the first user (device owner) is super (:418) — lockout recovery. `kBondAdminUser` is super while bonded (:365) |

Context adapters: `hasAdminPrivilege(ctx)` / `hasSuperAdminPrivilege(ctx)` —
`HardwareOne.cpp:655` and `:659` (both just `is*User(ctx.user)`).

Effective-rank helper: `userAccountRank()` `System_User.cpp:1237`.
Mutation rank guard (can't grant above your own / demote someone higher):
`userMutationAllowed()` `System_User.cpp:1557`.

### Synthetic / reserved identities

| Name | Where set | Effective tier |
|---|---|---|
| `"system"` | `runUnifiedSystemCommand` (`System_Utils.cpp:4961`), REST status facades | **bypasses authorizeCommand entirely** (:4303) |
| `"AuthBypass"` | serial `HardwareOne.cpp:2575`, OLED `OLED_Utils.cpp:2980`, BLE `Bluetooth.cpp:202` | passes the empty-identity check; `isAdminUser("AuthBypass")==false` → **User tier**. Reserved (`System_User.cpp:803`), cannot be created or logged into |
| `kBondAdminUser` = `"bond-admin"` | `System_User.h:78`; stamped in `v4_handle_cmd` `System_ESPNow.cpp:5779` | **Super Admin** for the life of a valid bond session |
| `""` (empty) | automations with no `createdBy`, some INTERNAL rows | User tier only (`isAdminUser("")` and `isSuperAdminUser("")` both return `false` on the length-0 guard) |

### Super-admin-only commands (11 registrations, `requiresSuperAdmin=true`)

`factoryreset` (System_Utils.cpp:2592) · `sdformat` (System_VFS.cpp:1203) ·
`serialrequireauth` (**registered twice** — System_Settings.cpp:241 *and* System_User.cpp:3385) ·
`displayrequireauth` (System_Settings.cpp:242) · `blerequireauth` (Bluetooth.cpp:2074) ·
`blesecret` (Bluetooth.cpp:2080) · `certgen` (System_WiFi.cpp:1619) ·
`espnowregenidentity` (System_ESPNow.cpp:15872) · `espnowusersync` (System_ESPNow.cpp:15943) ·
`espnowsetpassphrase` (System_ESPNow.cpp:15965).

Note there is **no `webrequireauth`** — web auth is not switchable, unlike serial/display/BLE.

---

## 2. SURFACE × AUTH-GATE MATRIX

`authorizeCommand` column = "does the command line pass through `executeCommand`?"

| # | Surface | Entry function (file:line) | Route to executeCommand | Pre-gate at the surface | authorizeCommand? | Identity stamped |
|---|---|---|---|---|---|---|
| 1 | **Serial CLI** | main-loop drain `HardwareOne.cpp:2477` | `submitAndExecuteSync` `HardwareOne.cpp:2590` | `gSettings.serialRequireAuth && !gSerialAuthed` → login-only (`:2497`); idle-logout at `:2489` | **YES** | `gSerialUser`, else `"AuthBypass"` (`:2575`), `SOURCE_SERIAL` |
| 2 | **Web `/api/cli`** | `handleCLICommand` `WebServer_Server.cpp:3172` | `submitAndExecuteSync` `:3253` | `tgRequireAuth` `:3175` + `webGuestAccessAllowed` `:3180` + 50 ms rate limit | **YES** | session user, `SOURCE_WEB` |
| 3 | **Web `/api/cli/batch`** | `handleCliBatch` `WebServer_Server.cpp:5025` | `submitAndExecuteSync` `:5093` (loop) | `tgRequireAuth` `:5027` + guest gate `:5031` | **YES** (per command) | session user, `SOURCE_WEB` |
| 4 | **Web CLI-equivalent helpers** | `executeUnifiedWebCommand` callers — `WebServer_Server.cpp:3836,4009,4032`; `WebPage_Maps.cpp:652,667,676,681`; `WebPage_Bond.cpp:1326,1392,1439,1452,1517` | `submitAndExecuteSync` via `System_Utils.cpp:4993` | per-handler `WEB_AUTH_OR_RETURN` / `WEB_AUTH_JSON_OR_RETURN` (`WebServer_Utils.h:72`, `:81`) | **YES** | session user, `SOURCE_WEB` |
| 5 | **Web `/register/submit`** | `handleRegisterSubmit` `WebServer_Server.cpp:3751` | `executeUnifiedWebCommand("userrequest …")` `:3836` | **none** (deliberately public) + IP lockout `:3765` | **YES** — and it **denies** (see G5) | `makeWebAuthCtx` leaves `ctx.user` **empty** |
| 6 | **Web `/api/bond/exec`** | `handleBondExec` `WebPage_Bond.cpp:1346` | prefixes `"remote:"` → `executeUnifiedWebCommand` `:1392` | `WEB_AUTH_JSON_OR_RETURN` `:1347` (any non-guest) | **YES but no-op** — see G1 | session user, `SOURCE_WEB` |
| 7 | **Web `/api/bond/cli/batch`** | `handleBondCliBatch` `WebPage_Bond.cpp:1472` | `"remote:"+cmd` → `executeUnifiedWebCommand` `:1517` | `WEB_AUTH_JSON_OR_RETURN` `:1473` + bond-master check `:1475` | **YES but no-op** — see G1 | session user, `SOURCE_WEB` |
| 8 | **Web REST status facades** | `runInternalStatusCmd` `WebServer_Server.cpp:2989`; `bleRunInternal` `WebPage_Bluetooth.cpp:34`; `espnowRunInternal` `WebPage_ESPNow.cpp:556` | **direct `executeCommand`** (`:2997`, `:41`, `:563`) | caller handler's `WEB_AUTH_*` | **bypassed by design** (`SOURCE_INTERNAL`/`"system"`) | `"system"` |
| 9 | **Web `/api/gps/tracks?live=…`** | `handleGPSTracksAPI` `WebPage_Maps.cpp:109` | **`executeCommandThroughRegistry`** `:129,133,134,135,141` | `WEB_AUTH_OR_RETURN` `:110` — but path is in the **guest GET allowlist** (`WebServer_Utils.cpp:387`) | **NO AUTH GATE FOUND** | n/a — handler called directly |
| 10 | **MQTT (local exec)** | `handleMQTTCommand` `System_MQTT.cpp:421` | `submitAndExecuteSync` `:544` | `isValidUser(user,pass)` per message `:472` | **YES** | JSON `user` field, `SOURCE_MQTT` |
| 11 | **MQTT (mesh route `room:`/`tag:`/`device:`)** | same handler, `System_MQTT.cpp:483–512` | **direct handler calls** `cmd_espnow_roomcmd(:494)` / `cmd_espnow_tagcmd(:501)` / `cmd_espnow_remote(:508)` | `isValidUser` only (`:472`) | **NO AUTH GATE FOUND** — see G2 | n/a |
| 12 | **ESP-NOW remote command** | RX `v4h_cmd` `System_ESPNow.cpp:2900` → deferred → `v4_handle_cmd` `System_ESPNow.cpp:5713` (dispatched `:8861`) | `submitCommandAsync` `:5931` | opcode flag `V4_OPC_FLAG_REQ_PAIRED` (`:4960`); **session-encryption mandatory** `:5739`; then `@BOND:` token via `validateBondSessionToken` `:5771`, **or** `isValidUser(user,pass)` `:5841` | **YES** | `kBondAdminUser` (super) on the bond path `:5779`, else the supplied username; `SOURCE_ESPNOW` |
| 13 | **BLE CLI characteristic** | `CmdRequestCallbacks::onWrite` `Bluetooth.cpp:489` → `processIncomingBLECommand` `:859` → `processBleCommandLine` `:673` | `submitCommandAsync` `:830` (and `:767` for login) | `bleScRequired()` plaintext reject `:871`; `gSettings.bleRequireAuth && !bleIsAuthed` `:797`; secure-channel gate for file-browser cmds `:811` | **YES** | `bleFillCommandAuth` `:186` → conn user, else `"AuthBypass"` `:202`; `SOURCE_BLUETOOTH` |
| 14 | **BLE secure-channel frames** | `bleScDeferredInbound` `Bluetooth.cpp:844` (runs via `submitDeferredToCmdExec`) | decrypted plaintext → `processBleCommandLine` `:850` | X25519/HKDF channel + the same `:797` auth gate downstream | **YES** (rejoins path 13) | as above |
| 15 | **G2 lens (hijack UI taps)** | BLE notify → `tapDispatcherEnqueue` `G2_Glasses.cpp:2722` → page handlers → `g2SubmitHijackCommand` `G2_HijackCmd.cpp:85` (93 call sites across 15 `G2_Page_*` files) | `submitCommandAsync` `:143` | **pair-time trust only** — refuses when `pairedByUser` is blank `:93`; no per-session credential | **YES** | `g2HijackAuthContext()` `:177` → `gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser`; `SOURCE_G2_GLASSES` |
| 16 | **G2 lens (sync direct-FS)** | `G2HijackCtxGuard` `G2_HijackCmd.cpp:214` | none — direct `VFS::*Guarded()` calls | same pair-time identity in task TLS | **N/A** (no command) — VFS perm layer instead | same |
| 17 | **OLED UI (buttons/gamepad)** | mode handlers → `executeOLEDCommand` `OLED_Utils.cpp:3005` / `executeOLEDCommandWithResult` `:3017` (146 call sites) | `submitAndExecuteSync` `:3010` / `:3022` | `shouldBlockForDisplayAuth()` `OLED_Utils.cpp:2901`; guest UI gates `oledModeAllowedForGuest` `:2908`, `oledGuestBlocksMutate` `:2957` | **YES** | `oledAuthContext()` `:2977` → `gLocalDisplayUser`, else `"AuthBypass"` `:2980`; `SOURCE_LOCAL_DISPLAY` |
| 18 | **OLED free-text CLI entry** | `OLED_Mode_CLIInput.cpp:161` | `executeOLEDCommandWithResult` | as row 17 | **YES** | as row 17 |
| 19 | **OLED direct-FS browser** | `oledFileBrowserAuthContext` `OLED_Mode_FileBrowser.cpp:52`, guard `:63` | none — direct VFS | as row 17 | **N/A** (VFS perms) | as row 17 |
| 20 | **Automations engine** | `queueAutomationSubCommand` `System_Automation.cpp:94`, fired from `executeConditionalCommand` `:894` | `submitCommandAsync` `:116` | none at dispatch; creation is admin-gated (`automationadd` `requiresAdmin=true`, `:4151`) and edit/delete re-check `createdBy` (`:1475`, `:1558`) | **YES** | `SOURCE_INTERNAL` + `auth.user = createdBy` (`:99`) — **not** `"system"`, so gates 3–5 still apply |
| 21 | **Boot autostart** | `HardwareOne.cpp:1879,1894,1897,1902,1913,1931,1942` | `runUnifiedSystemCommand` → `submitAndExecuteSync` | boolean settings only (`gSettings.<x>Enabled && ramFlushResolve(...)`) | **bypassed by design** (`"system"`) | `"system"` |
| 22 | **Voice / ESP-SR** | `executeVoiceCommandAsArmedUser` `System_ESPSR.cpp:176` (3 call sites in the recognition FSM) | `submitAndExecuteSync` `:206` | must be *armed*: `isVoiceArmed()` `:165`; arming requires a non-INTERNAL ctx with a non-empty user (`voiceArmFromContextInternal` `:152`) | **YES** | `gVoiceArmedUser`, `SOURCE_VOICE` |
| 23 | **Setup wizard / first-time setup** | `System_SetupWizard.cpp`, `OLED_SetupWizard.cpp`, `System_FirstTimeSetup.cpp` | **none** — no command execution found (grep for `executeCommand`/`submit*`/`dispatchCommand` in all three: no hits except an unused `extern` decl at `System_FirstTimeSetup.cpp:68`) | pre-account state | **N/A** | direct settings/account writes |
| 24 | **Migration `/api/restore`** | `WebServer_MigrationTool.cpp:815` (main server, gate-1 registered) and `:974` (restore-only server) | none — writes files directly | **intentionally unauthenticated**, triple-gated by registration lifecycle (`:812`, `:830`) | **N/A** | n/a |
| 25 | **"Scheduled tasks"** | no separate subsystem — the automation scheduler (`System_Automation.cpp:3642`) is the only scheduler and reuses row 20 | — | — | **YES** (via row 20) | — |
| 26 | **CLI mode / confirm resolution** | `cliModeDispatchInput` `System_Utils.cpp:4630` (inside executeCommand, *after* authorizeCommand); confirm mode `System_CLIConfirm.cpp:44` | consumed in-mode | authorizeCommand ran on the raw line (`"yes"`) | **YES but no-op** — see G3 | confirmer's ctx; the *action* runs under the requester's captured ctx |

### Non-command ESP-NOW ingress (mutating, gated by opcode flags not authorizeCommand)

Opcode table: `System_ESPNow.cpp:4938–5024`. Flags: `V4_OPC_FLAG_REQ_PAIRED` (0x01),
`REQ_BOND_MODE` (0x02), `REQ_AUTHENTICATED` (0x04), `REQ_SESSION_ENC` (0x08) —
defined at `System_ESPNow.cpp:2798–2811`, enforced in the dispatcher at `:5118`.

| Opcode | Flags | Extra inline gate | authorizeCommand? |
|---|---|---|---|
| `FILE_START/DATA/END/CANCEL` (`:4993–4996`) | PAIRED\|SESSION_ENC | — | **NO** (by design; not a command) |
| `FS_LIST/STAT/GET_*` (`:5007–5012`) | PAIRED\|SESSION_ENC | — | **NO** |
| `USER_SYNC` (`:4985`) | **0** | own gate: session-enc `:3549`, `espnowUserSyncEnabled` `:3557`, `isValidUser` `:3596`, `isAdminUser` `:3603` | **NO** (equivalent explicit gate present) |
| `SETTINGS_REQ` / `SCHEMA_REQ` / `MANIFEST_REQ` (`:5019,5020,5023`) | PAIRED\|BOND\|SESSION_ENC | — | **NO** |
| `TOPO_REQ/START/PEER` (`:4982–4984`) | **0** | — | **NO** |
| `TIME_SYNC` (`:4957`), `PAIR_*` (`:4967–4973`) | AUTHENTICATED | pairing is user-confirmed (accept screen) | **NO** |

---

## 3. GAPS FOUND DURING RECON

These are the auth-gate holes the security track should pick up. All were read in source.

### G1 — `remote:` / `@` prefix is an ungated cross-device escalation, reachable from EVERY surface · **CONFIRMED**

`executeCommand` strips a `remote:` / `remote ` / `@` prefix at `System_Utils.cpp:4505-4515` and
forwards the inner command to the bonded peer, where it runs as `kBondAdminUser` (Super Admin).
The wrapper line never resolves in `findCommand` (there is **no** command named `remote`;
verified by grep over all registration tables), so `commandRequiresAdmin`/`SuperAdmin` both
return `false` and gates 4+5 of `authorizeCommand` never fire. The code says so explicitly:

```cpp
// System_Utils.cpp:4535
// Bonding treats the two devices as ONE unit, so the bond session token IS the trust —
// the local caller's role is NOT re-checked here
```

Effect: **any identity that clears gates 2 and 3 — i.e. any non-guest, including
`"AuthBypass"` on a serial/OLED/BLE box with require-auth turned off — can run arbitrary
commands on the bonded peer with Super Admin privilege.** The only guard is the nested-wrapper
reject at `:4519` (which stops `remote:remote:X` looping back to super on the origin).

Reachable from: serial, `/api/cli`, `/api/cli/batch`, `/api/bond/exec`, `/api/bond/cli/batch`,
OLED (`OLED_Mode_Remote.cpp:454,458,484,491`, `OLED_RemoteSettings.cpp:262`), BLE, G2, MQTT, voice, ESP-NOW.
Guests *are* blocked (gate 3 fires first, `commandAllowedForGuest` rejects `@…`).

### G2 — MQTT mesh-route path calls admin-only handlers directly · **CONFIRMED** (KNOWN, status changed)

`System_MQTT.cpp:483–512` calls `cmd_espnow_roomcmd` (`:494`), `cmd_espnow_tagcmd` (`:501`),
`cmd_espnow_remote` (`:508`) as raw function pointers. All three are registered with
`requiresAdmin = true` (`System_ESPNow.cpp:15922, 15923, 15931`), and none of that is checked —
`authorizeCommand` is never reached on this branch. Any MQTT client with *any* valid
credential (including a **guest** account — `isValidUser` at `:472` does not consult role)
can dispatch mesh commands. Mitigation in depth: the dispatched commands carry
target-device credentials that are verified on the *target*, so this is a local-policy
bypass, not direct RCE.

Memory backlog listed this as "MQTT bridge skips authorizeCommand". **Status update:** the
*local* MQTT exec path was fixed (routed through `submitAndExecuteSync` at `:544`, see
`docs/CMD_ROUTING_MQTT_VOICE_2026-07-24.md` §2b) — but that doc explicitly states the
mesh-routing block was "unchanged", and it still is. The finding is half-fixed.

### G3 — confirm-mode `yes` is not re-authorized; any surface can resolve another user's pending destructive prompt · **CONFIRMED**

`sConfirm` is a **single global slot** with no owner check (`System_CLIConfirm.cpp:18–29`).
`confirm_onInput` (`:44`) fires the stored callback for any line matching `yes/y/true/1/on`.
Because `cliModeDispatchInput` runs *inside* `executeCommand` at `System_Utils.cpp:4630` —
i.e. **after** `authorizeCommand` — the gate sees the literal string `"yes"`,
`findCommand("yes")` returns `nullptr`, and no admin/super check applies.

Requesters: `factoryreset` (**super-admin only**, `System_Utils.cpp:2241`),
`userdelete` (`System_User.cpp:2053`), `filedelete` (`System_Filesystem.cpp:1183`).

Concrete escalation: a super-admin types `factoryreset` on serial and walks away.
A plain `user` on the web CLI (or an `AuthBypass` OLED tap, or an ESP-NOW peer) sends `yes`.
`factoryreset_confirmed` deletes `/system/users/users.json` and reboots into the setup wizard.
The destructive action itself runs under the *requester's* captured `AuthContext`
(`s_pendingFiledeleteCtx`, `System_Filesystem.cpp:1176`) — so the permission layer is
satisfied by the original admin, and the audit line attributes the decision to the confirmer
(`System_CLIConfirm.cpp:106`). Nothing blocks the low-privilege confirmer.

### G4 — `login <user> <pass> <transport>` grants a session on a *different* transport · **CONFIRMED**

`cmd_login` (`System_Utils.cpp:5202`) is registered `requiresAdmin=false`
(`System_User.cpp:3383`) and takes a free-text transport argument (`:5214`) mapped to
`SOURCE_SERIAL` / `SOURCE_LOCAL_DISPLAY` / `SOURCE_BLUETOOTH` (`:5219–5226`).
`loginTransport` (`System_User.cpp:439`) then sets the *global* session state for that
transport — `gSerialAuthed`/`gSerialUser` (`:466`) or `gLocalDisplayAuthed`/`gLocalDisplayUser`
(`:472`). So a command arriving over ESP-NOW / MQTT / BLE / voice can log the **physical OLED
console** in as any account whose password it holds, and the OLED then shows that session to
whoever is standing at the device.

Mirror: `cmd_logout` (`System_Utils.cpp:5242`) accepts `serial|display|bluetooth|g2` and is
**allowed for guests** (`commandAllowedForGuest`, `System_Utils.cpp:4289` permits `logout`
with any argument). A guest on serial/BLE can therefore run `logout display` (kick the OLED
session), `logout bluetooth` (`bleRevokeAllSessions`, `System_User.cpp:522`), or `logout g2`
(clears the lens `pairedByUser` stamp, `:513`).

### G5 — public self-registration is dead: `/register/submit` is denied by its own auth gate · **CONFIRMED** (functional, not security)

`handleRegisterSubmit` (`WebServer_Server.cpp:3751`) builds `AuthContext ctx = makeWebAuthCtx(req)`
at `:3831`. `makeWebAuthCtx` (`WebServer_Server.h:246`) sets only transport/opaque/path/ip —
**`ctx.user` stays empty** — and `tgRequireAuth` is never called on this handler (correctly,
it is a public page). It then runs `userrequest <u> <p> <p>` through
`executeUnifiedWebCommand` (`:3836`).

`authorizeCommand` gate 2 (`System_Utils.cpp:4311`) rejects any empty-user non-INTERNAL
context unless the command is literally `login`:

```cpp
    if (!cmdName.equalsIgnoreCase("login")) {
      snprintf(out, outSize, "Error: Authentication required.");
```

`userrequest` is not `login`, so every registration attempt returns
`"Error: Authentication required."`, `ok` is false, and the fallback
`ok = ok || (out.indexOf("Request submitted for") >= 0)` at `:3837` cannot rescue it.
The user sees "Unable to submit request." and `recordFailedLogin` fires against their IP.

### G6 — guest can start GPS + filesystem logging via a GET · **CONFIRMED**

`/api/gps/tracks` is on the guest allowlist (`WebServer_Utils.cpp:387`), and
`webGuestAccessAllowed` (`:401`) only blocks non-GET methods for guests. But
`handleGPSTracksAPI` (`WebPage_Maps.cpp:109`) treats query params as verbs and, for
`?live=start`, calls `executeCommandThroughRegistry` four times (`:129,133,134,135`) —
`opengps`, `sensorlog format track`, `sensorlog sensors gps`,
`sensorlog start /logging_captures/tracks/live.csv 1000` — and once for `?live=stop` (`:141`).
`executeCommandThroughRegistry` invokes `found->handler()` directly (`System_Command.cpp:263`):
**NO AUTH GATE FOUND**. Both commands happen to be `requiresAdmin=false`
(`i2csensor_pa1010d.cpp:737`, `System_SensorLogging.cpp:2074`), so this is a guest/view-only
violation (a "view-only" account powers a sensor and starts writing files), not an admin bypass.

### G7 — G2 lens identity silently falls back to the device owner (usually Super Admin) · **CONFIRMED, by design — flagging the blast radius**

`g2HijackAuthContext()` (`G2_HijackCmd.cpp:177`) reads `pairedByUser`. When blank it calls
`bleStampPairedByIfBlank` (`:193`), whose resolver `bleResolveStampUsername`
(`BLE_Peers.cpp:144`) falls back through TLS identity → live OLED session → live serial session →
**`getDeviceOwnerUsername()`** (`:172`). The owner is the first users.json entry, which
`isSuperAdminUser` treats as super under the no-explicit-super fallback (`System_User.cpp:418`).
Result: an unowned lens that reconnects at boot can be homed to the device owner, and every
subsequent lens tap runs with that account's privileges — with **no per-session credential
check anywhere** (`tgRequireAuth` `System_User.cpp:187` short-circuits `SOURCE_G2_GLASSES` to
"pairing IS auth"). Physical possession of the paired glasses ≈ owner-level CLI.

### G8 — `serialrequireauth` is registered twice · **CONFIRMED** (hygiene)

`System_Settings.cpp:241` and `System_User.cpp:3385` both register a `"serialrequireauth"`
entry, each with a different handler symbol name and help text. `findCommand`
(`System_Command.cpp:110`) does longest-*name* match and returns the **first** table hit at
that length, so which one runs depends on registry ordering. Both are
`requiresAdmin=true, requiresSuperAdmin=true`, so there is no privilege consequence today —
but the duplicate makes the auth-posture command's behaviour registry-order-dependent.

---

## 4. Cross-references to prior docs (do not re-file as new)

| Topic | Prior doc | Status |
|---|---|---|
| Task B0 (this map) | `docs/OVERNIGHT_AUDIT_BACKLOG.md:94` | now done |
| MQTT/voice routed through `cmd_exec` | `docs/CMD_ROUTING_MQTT_VOICE_2026-07-24.md` | implemented; §2b explicitly leaves the mesh-route block unchanged → G2 |
| `authorizeCommand` double `findCommand` walk | `docs/HOT_PATH_HEAP_AUDIT.md:238`, `docs/PRE_1_0_CODE_HEALTH_AUDIT.md:64` | perf, still open |
| "enforcement is real (authorizeCommand …)" | `docs/CONTROLS_WRITE_INTEGRITY.md:108` | consistent with this map |
| Uneven web authorization granularity | `docs/WEB_API_INVENTORY.md:2173` | overlaps G6; that entry names `/espnow` and bond endpoints, not `/api/gps/tracks` |
| G2 tap → `authorizeCommand` as paired user | `docs/G2_INTERACTIVE_SETTINGS_PLAN.md:60` | consistent; G7 adds the owner-fallback blast radius |
| Backlog items to verify | `docs/OVERNIGHT_AUDIT_BACKLOG.md:98` (B4: FTS AuthBypass, MQTT bridge) | MQTT half resolved (G2). **FTS AuthBypass not reproduced**: the setup wizard executes no commands at all (row 23), so there is no FTS→`executeCommand` path to bypass — the phrase most likely refers to the `"AuthBypass"` sentinel, which resolves to User tier, not admin |

---

## 5. Board gating

Nothing in this map is board-specific. The surfaces are gated by *feature* macros, not board
macros: `ENABLE_HTTP_SERVER`, `ENABLE_ESPNOW`, `ENABLE_BONDED_MODE`, `ENABLE_BLUETOOTH`,
`ENABLE_G2_GLASSES`, `ENABLE_OLED_DISPLAY`, `ENABLE_MQTT`, `ENABLE_AUTOMATION`,
`ENABLE_MIGRATION_TOOL`, plus the ESP-SR voice gate. `tgRequireAuth` has a full
`#else` stub for `ENABLE_HTTP_SERVER == 0` (`System_User.cpp:227`) that keeps the serial /
display / G2 branches and drops the web branch — the gate is not weakened in that build.
G1/G3/G4/G5 apply to every board that compiles the corresponding feature.
