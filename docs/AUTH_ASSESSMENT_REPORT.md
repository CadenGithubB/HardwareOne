# HardwareOne ESP32: Authentication & Identity Assessment

This document is a research-only assessment of the authentication / identity subsystem as it stands after the per-task TLS identity refactor (Stages 1-3). It surveys `components/hardwareone/System_AuthIdentity.{cpp,h}`, `System_User.{cpp,h}`, `WebServer_Server.{cpp,h}`, `WebServer_Utils.{cpp,h}`, `System_VFS.{cpp,h}`, `System_ESPNow.cpp`, `G2_HijackCmd.{cpp,h}`, `G2_Glasses.cpp`, `Bluetooth.{cpp,h}`, `HardwareOne.cpp` (boot + serial CLI loop), `OLED_Mode_Auth.cpp`, `OLED_Mode_ChangePassword.cpp`, `OLED_Utils.cpp`, `System_Settings.cpp`, `System_Debug.cpp`, and a smattering of `WebPage_*.cpp` for tgRequireAuth usage. Goal: understand each pathway end-to-end, surface inconsistencies and shortcomings, and propose concrete cleanups.

---

## 1. Mechanisms at a glance

| Mechanism | What it gates | Storage | Lifecycle |
|---|---|---|---|
| Web session cookie | All non-public HTTP endpoints | `gSessions[MAX_SESSIONS=2]` in PSRAM | TTL 24h, refreshed-on-use, expired on boot-ID mismatch (`WebServer_Server.cpp:162,516`) |
| HTTP Basic Auth fallback | Same endpoints, used by Migration Tool | None (per-request) | Per-request; subject to lockout (`WebServer_Server.cpp:448-470`) |
| Brute-force / IP lockout | Web login + Basic Auth | `sLoginAttempts[MAX_LOGIN_ATTEMPT_ENTRIES=8]` EXT_RAM | Tiered (5/30s, 10/5m, 20/30m), 10-min window (`WebServer_Server.cpp:692-742`) |
| Permanent IP ban | Web login + every guarded request | `sIpBans[MAX_IP_BANS=16]` EXT_RAM, persisted AES-encrypted to `/system/users/ip_bans.json` | Admin-managed via `cmd_ban`/`unban` (`WebServer_Server.cpp:769-901`) |
| Per-task TLS identity | All command execution + VFS guarded calls | TLS slot 1 (`System_AuthIdentity.cpp:38`) | Installed by `ExecIdentityGuard` in `executeCommand` (`System_Utils.cpp:2762`) |
| `gIdentityGeneration` clock | Auth-derived caches (FileManager) | `std::atomic<uint32_t>` | Bumped on user add/approve/delete/promote/demote + BLE pair stamp |
| Session-revoke fan-out | All transports | `revokeUserSessions` (`System_User.cpp:289`) | Called on delete/demote/password-change/password-reset |
| Serial auth | Serial CLI commands | `gSerialAuthed`, `gSerialUser` | Inlined in main loop (`HardwareOne.cpp:1700-1731`), no rate limit |
| Local display auth | OLED CLI + UI | `gLocalDisplayAuthed`, `gLocalDisplayUser` | Login at `OLED_Mode_Auth.cpp:184`; no rate limit |
| BLE auth | BLE GATT CLI commands | `gBLEState->connections[].authed/user` per-conn | Login deferred to `cmd_exec_task` (`Bluetooth.cpp:466`); 15-min idle timeout (`Bluetooth.cpp:105`); no rate limit |
| ESP-NOW user/password auth | Remote `user:pass:cmd` payloads | None (one-shot) | `v3_handle_cmd` (`System_ESPNow.cpp:3741-3766`); no rate limit |
| ESP-NOW bond session token | `@BOND:<token>:cmd` payloads | `gEspNow->bondSessionToken[16]` RAM-only | HMAC-SHA256(passphrase, sorted MAC pair) recomputed on bond connect (`System_ESPNow.cpp:821-871`) |
| G2 glasses hijack identity | Tap-driven commands from lenses | `gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser` (persisted) | Built fresh per submission (`G2_HijackCmd.cpp:125`); installed via `ExecIdentityGuard` for direct-FS work (`G2_HijackCmd.cpp:202`) |
| `VFS::systemAuth(reason)` | Internal trusted FS operations | None (per-call) | Constructs a `user="system"`, `transport=SOURCE_INTERNAL` `AuthContext` (`System_VFS.cpp:862`) |

Reserved-username sentinels in the audit trail: `"system"` (real internal work), `"AuthBypass"` (physical user with `*RequireAuth=off`). Both rejected by `adminCreateUser` (`System_User.cpp:508-516`).

---

## 2. Per-surface walkthroughs

### 2.1 Web session auth (`WebServer_Server.cpp`)

**Data structures.** `SessionEntry` (`WebServer_Server.h:38-70`) carries `sid`, `user`, `bootId`, `createdAt/lastSeen/expiresAt`, `ip`, plus per-session SSE notice + event ring buffers, a `sockfd` for force-close, and a `revoked` flag (graceful tombstone for notice delivery).

**Cookie format.** `session=<sid>; Path=/` — no `HttpOnly`, no `Secure`, no `SameSite`, no `Max-Age`. Set with a static 96-byte buffer (`WebServer_Server.cpp:245,297`). The cleared cookie on logout *does* set `HttpOnly; SameSite=Strict; Max-Age=0` (`WebServer_Server.cpp:425`) — asymmetric vs. the cookie that was set. Token is `esp_random()×3 + millis()` formatted as 32-hex (`WebServer_Utils.cpp:327-339`); 96 bits effective random.

**Login flow.** `handleLogin` (POST, `WebServer_Server.cpp:3391`):
1. Reject if `isIpBanned` (403).
2. Reject if `isLoginLocked` (re-render form with cooldown message).
3. Validate via `isValidUser(u, p)` → PBKDF2-HMAC-SHA256, 10000 iter, salt = `getDeviceEncryptionKey()`.
4. On success: `clearLoginAttempts`, audit log, `setSession`, 303 → `/dashboard` (or `/account/password-change` if `userMustChangePassword`).
5. On failure: `recordFailedLogin`, audit log, re-render with "N attempts remaining" warning if within 2 of lockout.

**`isAuthed` (`WebServer_Server.cpp:434-553`)** is the canonical checker:
1. Ban check (denied unconditionally — banned IPs cannot even read static assets).
2. If no `session=` cookie: try Basic Auth via `decodeBasicAuth`, lockout-aware (Migration Tool path).
3. Session lookup by SID.
4. Boot-ID match (`gSessions[idx].bootId != gBootId` → store "expired due to restart" reason, clear).
5. `revoked` flag, expiry, refresh-on-use, return user.

**`setSession` (`WebServer_Server.cpp:227-304`)** has three branches:
1. Same-user-same-IP reuse path → extend expiry + re-Set-Cookie.
2. Same-user-different-IP eviction → "you were signed out because you logged in from another device" stored for the old IP, sockfd force-close, slot freed.
3. Fresh session in a free slot (or evict oldest).

**`MAX_SESSIONS = 2`.** Effectively per-user limit too: only one slot is permitted per user; the IP-reuse path prevents needless rotation but it's still a 2-session-total cap across the entire device.

**Logout reasons** are a separate per-IP ring (`gLogoutReasons[MAX_LOGOUT_REASONS=8]`) so the login page can explain why the user was kicked. 30-second expiry, rate-limited by reason equality (`WebServer_Server.cpp:584-647`).

### 2.2 `tgRequireAuth(ctx)` vs `isAuthed(req, user)`

`tgRequireAuth` (`System_User.cpp:90-128`) is the transport-generic gate:

| Transport | Behaviour |
|---|---|
| `SOURCE_WEB` | Calls `isAuthed(req, user)`; on failure emits `sendAuthRequiredResponse` (JSON 401 for `/api/*`, 303 → `/login` for HTML), then returns false. Stamps `ctx.user`, `ctx.ip`, logs success via `logAuthAttempt`. |
| `SOURCE_SERIAL` | Reads `gSerialAuthed`. If `gSettings.serialRequireAuth` is off, returns true unconditionally. |
| `SOURCE_LOCAL_DISPLAY` | Reads `gLocalDisplayAuthed` via `shouldBlockForDisplayAuth()`. |
| Other (INTERNAL/ESPNOW/BLUETOOTH/MQTT/VOICE) | Returns true — assumes upstream already authenticated. |

The `WEB_AUTH_OR_RETURN(req, ctx)` macro (`WebServer_Utils.h:69`) is the standard idiom and is used by ~50 handlers. Three direct callers of `isAuthed` remain:

- `WebServer_Server.cpp:3033` — `handleNotice` wants a JSON 401 (not the HTML 303) so it bypasses `tgRequireAuth` and writes its own response.
- `WebServer_Server.cpp:3408` — `handleLogin` GET uses `isAuthed` to detect "already logged in" and redirect to `/dashboard` rather than re-render the form (deliberately doesn't write a 401).
- `System_User.cpp:96` — inside `tgRequireAuth` itself.

**Semantic difference.** `tgRequireAuth` always *writes a response on failure* (for WEB); `isAuthed` is pure read. Callers that need a custom failure response (handleNotice, the "already authed" check in handleLogin) reach past `tgRequireAuth`. That's the entire reason for the two-function shape. Both call into the same session check, so there's no policy drift — but the contract should be documented (see §4).

### 2.3 Brute-force lockout / IP bans

**Per-IP windowed tracking** (`WebServer_Server.cpp:653-758`):
- Storage: 8 fixed `LoginAttemptEntry` slots in EXT_RAM, char-array IP (no `String` heap).
- Window: 10 min. Failures inside the window accumulate; window expiry resets the slot.
- Tiers: 5 → 30s, 10 → 5m, 20 → 30m. `lockoutDurationMs` is monotonic in `failCount`, so once you cross tier 1 the next failure immediately re-locks at the same-or-higher tier even after the lock clears.
- Slot eviction when all 8 slots are full picks the oldest `windowStart` — so a sustained attack from 8 IPs cannot exhaust the table for a 9th.
- `sTotalFailedLogins` is a separate monotonic uint32 surfaced in the OLED Web Stats card via `getTotalFailedLoginCount()` (intentionally not reset by `clearLoginAttempts`).

**Audit log.** `LOG_FAIL_FILE = "/system/sys_logs/failed_login.log"` (`System_Debug.cpp:3215`). Only login events get a line (`logAuthAttempt` filters by `path.indexOf("/login") >= 0` or `reason="Login successful"` at `WebServer_Server.cpp:1512`); per-page tgRequireAuth-success calls don't pollute the file.

**Permanent IP bans** (`WebServer_Server.cpp:769-901`):
- Loaded lazily on first `isIpBanned`.
- Persisted AES-encrypted (`decryptString`/`encryptString`) in `/system/users/ip_bans.json`. The encryption key derives from the same device key as password hashes. Plain-text legacy entries are accepted on load and re-encrypted on next save.
- `banIp` also kicks any active sessions from that IP (sets `revoked`, sends SSE notice) and clears any brute-force tracking for that IP.

**Surfaces NOT covered by brute-force / lockout:**

| Surface | Path | Brute-force? | Audit log? |
|---|---|---|---|
| Web login | `handleLogin` | YES | YES |
| Web Basic Auth | `isAuthed` cookie-miss fallback | YES | NO (only on path containing `/login`) |
| Serial CLI `login` | `HardwareOne.cpp:1700-1731` (inlined, **does not call** `cmd_login`) | **NO** | NO |
| OLED login | `OLED_Mode_Auth.cpp:184` (`loginTransport`) | **NO** | NO |
| BLE `login` | `Bluetooth.cpp:584-602` → `cmd_login` on cmd_exec_task | **NO** | YES (audit logs the cmd, but no IP-equivalent rate limit) |
| ESP-NOW `user:pass:cmd` | `System_ESPNow.cpp:3754` | **NO** | NO |
| ESP-NOW user-sync admin check | `System_ESPNow.cpp:2598-2603` | **NO** | NO |
| Web password-change current-pwd check | `WebServer_Server.cpp:2466` | **NO** | NO |

### 2.4 Per-task TLS identity (`System_AuthIdentity.{cpp,h}`)

**Slot allocation.** TLS slot 1 (slot 0 is owned by `pthread`'s `pthread_local_storage.c` — must not be touched). Slot count is raised to 4 in sdkconfig for headroom. Lazy allocation on first `ExecIdentityGuard` construction; `initAuthIdentityForCurrentTask()` is a documentation-only helper called from `app_main`.

**Readers.** `currentAuthContext()`, `currentExecUser()`, `currentExecIsAdmin()` — return the calling task's slot (or the anonSentinel for tasks that never installed anything). Used by ~30 callsites across the tree, primarily `VFS::*Guarded` callsites and audit-log paths.

**Writers.**
- `ExecIdentityGuard guard(ctx);` — RAII install/restore.
- `SYSTEM_IDENTITY_SCOPE("reason")` — macro that builds a SYSTEM identity and installs it.

The only producer of an installed identity is `executeCommand` (`System_Utils.cpp:2762`) and the synchronous fallback in `submitAndExecuteSync` (`System_Utils.cpp:3014`). Auxiliary worker contexts (G2 hijack tap, live-page builder) install their own guard before invoking VFS — e.g. `G2_Glasses.cpp:9260`, `:9835`, `:13708`, `:14744`, `:15124`, `:15215` and `G2_HijackCmd.cpp:202`.

**Identity-generation clock.** `gIdentityGeneration` is bumped at:
- `bleStampPairedByIfBlank` (BLE peer ownership transfer)
- `cmd_user_add`, `cmd_user_approve` (account becomes real)
- `cmd_user_delete` (account gone)
- `cmd_user_promote` / `cmd_user_demote` (admin transition)

It is intentionally NOT bumped on login/logout, password change/reset, or `userrequest`/`userdeny`. See the header block at `System_AuthIdentity.h:64-189` — the design philosophy is documented in unusual depth there.

**Stage 3 additions.** `currentCommandContext()` and `currentCaptureState()` moved per-task too (`System_AuthIdentity.h:223-232`), fixing a cross-task output-routing bug where a non-cmd_exec broadcast got captured into whatever WebSocket session held the captureBuf last.

**No relics.** `gExecAuthContext`/`gExecUser`/`gExecIsAdmin` are fully removed — only the header comment block at `System_AuthIdentity.h:6-8` and `HardwareOne.cpp:381` mention them historically.

### 2.5 User store (`System_User.{h,cpp}`, 3192 LOC)

`users.json` schema (top of file): `{ version, bootCounter, nextId, users: [{ id, username, role, createdAt, createdBy, banned, lastSeen, ... }] }`. Passwords live in `/system/users/<id>/settings.json` under `password` (text PBKDF2 hash) and `gamepad_password` (pattern PBKDF2 hash), keyed by the integer user-id from `users.json` (`System_User.cpp:454-474, 798-826`).

**Pending users** (`/system/users/pending_users.json`) — array of registration requests with hashed password baked in at request time. `cmd_user_approve` moves the entry into `users.json` and writes the pre-hashed password into the new per-user settings.

**Role check.** `isAdminUser` (`System_User.cpp:156`) is hand-rolled string-index JSON parsing, with a fallback: "first user without role is admin." Two paths to admin: explicit `role=admin` in the JSON, or being the first user. This means demoting the first user to non-admin can leave them as admin via the fallback unless another `role=admin` row exists.

**`revokeUserSessions`** (`System_User.cpp:289-340`) is the canonical fan-out across all transports:
- Walks `gSessions[]` skipping `exceptSid`.
- Clears serial / local-display if their user matches, skipping `exceptTransport`.
- Calls `bleRevokeUserSessions` for BLE.

The protocol-level distinction between "bump clock" and "revoke sessions" is documented inline at `System_AuthIdentity.h:146-167` and the cmd-by-cmd annotations at `System_User.h:160-179`.

### 2.6 Password change flow

Three entry points converge on `setUserPassword`:

| Caller | Auth check | Calls `revokeUserSessions`? | Notes |
|---|---|---|---|
| Web `handlePasswordChangePage` POST | `WEB_AUTH_OR_RETURN`, then `isValidUser(ctx.user, current)` | **NO** | `WebServer_Server.cpp:2418-2490`. Bug: missing the revoke step. |
| CLI `cmd_user_changepassword` | `isValidUser(username, current)` where `username = getTransportUser(SOURCE_LOCAL_DISPLAY)` | YES (with caller-session exception) | `System_User.cpp:1666-1720`. Bug: always uses LOCAL_DISPLAY transport user — wrong for web, serial, BLE callers. |
| CLI `cmd_user_resetpassword` (admin) | Implicit (admin-only command + admin authz layer) | YES (no exception — target gets kicked everywhere) | `System_User.cpp:1722-1757`. Correct. |
| OLED `OLED_Mode_ChangePassword.cpp` | UI requires `isTransportAuthenticated(SOURCE_LOCAL_DISPLAY)` | YES (via `userchangepassword` invocation) | `OLED_Mode_ChangePassword.cpp:262-265`. Uses `executeOLEDCommandWithResult` so the command runs under the OLED identity. |

The OLED path is the *only* one that uses `cmd_user_changepassword` correctly, because it happens to install the OLED transport identity before submitting. A web POST to `/api/cli` of `userchangepassword …` would hit the broken `getTransportUser(SOURCE_LOCAL_DISPLAY)` branch — that returns the OLED user (or empty if no OLED login) regardless of who's calling.

The web POST handler at `WebServer_Server.cpp:2418` re-implements the entire flow inline rather than calling `cmd_user_changepassword`, which is why the bug is invisible from the web UI. The two implementations have **drifted**:
- Web validates and writes the password but never revokes other sessions.
- CLI revokes other sessions but uses the wrong user lookup.

### 2.7 ESP-NOW bond session tokens

Covered in `BOND_CAPABILITIES_REPORT.md`. From an auth-system perspective:

- Token = `SHA256(passphrase || sortedMacPair)`, truncated to 16 bytes, stored in RAM only on `gEspNow->bondSessionToken` (`System_ESPNow.cpp:821-871`).
- Recomputed on bond connect (`System_ESPNow.cpp:2914, 4746, 6347`); cleared on disconnect (`clearBondSessionToken`).
- Validated on incoming `@BOND:<token>:cmd` frames (`System_ESPNow.cpp:3701-3740`).
- **Independent from web sessions** — no overlap with `gSessions[]`, no boot-ID coupling, no per-user identity. Once a peer can compute the token, every `remote:` command they send executes as `username="espnow"` on this device.
- No expiry beyond passphrase-change or peer disconnect. No rotation.
- Companion path (traditional `user:pass:cmd`) at `System_ESPNow.cpp:3741-3766` directly validates `isValidUser` with no rate limit and stamps the AuthContext with the supplied user — so a real user identity *is* available for traditional auth.

The user-sync admin verification (`System_ESPNow.cpp:2598-2610`) uses `isValidUser` + `isAdminUser` on plain-text creds passed in the JSON payload. Also unrate-limited.

### 2.8 G2 glasses hijack identity (`G2_HijackCmd.{cpp,h}`, `G2_Glasses.cpp`)

Two distinct entry points share `g2HijackAuthContext()`:

1. **Async submission** via `g2SubmitHijackCommand` (`G2_HijackCmd.cpp:86`). Builds a `Command` with `transport=SOURCE_LOCAL_DISPLAY`, `user=gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser`, submits to `cmd_exec_task` via `submitCommandAsync`. The cmd_exec task then runs `executeCommand` which installs the identity via `ExecIdentityGuard`.

2. **Synchronous direct-FS work** from a BLE notify callback. Because `cmd_exec_task` is not in the loop here, `G2HijackCtxGuard` (`G2_HijackCmd.cpp:202`) installs the same identity directly into the calling task's TLS slot. Used at `G2_Glasses.cpp:13708, 14744, 15124, 15215`.

The "revisit TODO" mentioned in the task context (`G2_HijackCmd.cpp:115`) was **resolved** — the comment now explains the SOURCE_INTERNAL → SOURCE_LOCAL_DISPLAY change (commit history shows it landed inline with the per-task TLS work):

```cpp
// SOURCE_LOCAL_DISPLAY: the lens IS a local display (just BLE-attached
// rather than wired). Matches g2HijackAuthContext() so command-dispatched
// and direct-FS hijack work resolve to the same identity. Was previously
// SOURCE_INTERNAL with a "revisit" TODO — SOURCE_INTERNAL only matters
// for ANON/SYSTEM resolution when user=="system", which pairedByUser
// never is, so the change is purely cosmetic but removes a misleading
// transport label from audit lines.
```

**Stuck-state detector** (`G2_HijackCmd.cpp:180-193`): fires once per boot if `mac1` is set + `autoConnect=true` but `pairedByUser` is blank. Recovery: run `bleautoconnect g2-glasses on` from an authenticated CLI to stamp the field via `bleStampPairedByIfBlank`.

### 2.9 `VFS::systemAuth(reason)` sentinel

`VFS::systemAuth("purpose")` (`System_VFS.cpp:862-871`) builds a fresh `AuthContext` with `transport=SOURCE_INTERNAL`, `user="system"`, `path="system:purpose"`, `ip="local"`. It is the canonical way for internal trusted code to perform FS operations that bypass user-level VFS permission checks.

Conceptually it's a *capability* (a value you pass per call), not a privilege escalation that lingers on the calling task. The `SYSTEM_IDENTITY_SCOPE(purpose)` macro (`System_AuthIdentity.h:54`) is the other form — installs SYSTEM as the TLS identity for a scope. Both exist; both produce equivalent permission outcomes, but they serve different needs:

- `VFS::systemAuth(reason)` — one-off, pass-through call. Cheap (3 String assignments). Used when a single guarded call needs system rights but the surrounding code should keep running under the caller's identity.
- `SYSTEM_IDENTITY_SCOPE` — installs into TLS for the whole scope. Use when there's a block of work that all needs system rights, and you don't want to plumb the ctx through every helper.

The header comment at `System_VFS.h:152-168` enumerates valid uses (system-owned databases, boot-time cert reads, log rotation) and explicitly calls out misuse ("I'm a CLI handler" → use `currentAuthContext()`; "I'm in a web request" → use `makeWebAuthCtx(req)`).

### 2.10 Boot-ID session invalidation

`gBootId = (uint32_t)(chipId>>32) || (uint32_t)chipId || "_" || millis()` set at `HardwareOne.cpp:1024`. Embedded into each session at creation (`WebServer_Server.cpp:276`). Compared in `isAuthed` (`WebServer_Server.cpp:516`) — mismatch ⇒ stored cookie was issued by a previous boot ⇒ kill the session and store a "session expired due to system restart" reason for the IP.

Diagnostic toggle: `debugauthbootid` — settings tooltip says "Logs the per-boot auth ID that invalidates all sessions across a reboot. Useful for diagnosing unexpected logouts" (`WebPage_Settings.h:1079`). Enabling it routes `DEBUG_AUTHF` lines that print the bootId comparison through the debug subsystem.

**Note**: `gSessions[]` itself lives in PSRAM but is not persisted, so the boot-ID check is belt-and-braces — even without it, the in-RAM table is empty after reboot. The boot-ID matters for the *client cookie*: it lets the server tell apart "stale cookie from last boot" (show "system restart" message) from "bogus cookie that never existed" (silent rejection).

---

## 3. Cross-cutting concerns

### 3.1 Identity propagation across task hops

Every cross-task hop in the auth subsystem now uses one of two well-defined idioms:

1. **Submit through `cmd_exec_task`.** The submitting task fills the `Command.ctx.auth` AuthContext; `executeCommand` calls `ExecIdentityGuard identity(ctx)` (`System_Utils.cpp:2762`) which installs into cmd_exec's TLS slot. Restores on return.
2. **Run synchronously, install guard locally.** When a BLE notify callback or G2 hijack handler must do guarded FS work on its own task, it constructs an `ExecIdentityGuard` (or `G2HijackCtxGuard`) before any `VFS::*Guarded` call. Restores on scope exit.

`getSlotReadOnly()` (`System_AuthIdentity.cpp:74-80`) returns the anonSentinel for tasks without a slot, so a stray `currentAuthContext()` from an uninstrumented worker degrades to ANON instead of leaking another task's identity. That's the structural win of Stage 1.

### 3.2 Password hashing

`hashUserPassword` (`System_User.cpp:402-437`) — PBKDF2-HMAC-SHA256, 10000 iterations, 32-byte output, **salt = `getDeviceEncryptionKey()`** (eFuseMac || flashUid). Storage format: `PBKDF2:10000:<hex>`.

`verifyUserPassword` re-hashes the input and compares the full strings. There's a String allocation per check (`String inputHash = hashUserPassword(...)`) so it isn't constant-time — but the hash itself is 10000 iterations, so timing attacks against the comparison are not the weak link.

**Salt is per-device, not per-user.** Two users with the same password on the same device produce identical hashes, and an attacker with a single password→hash rainbow table for this device's salt cracks every account that shares that password in O(1) lookups after one offline PBKDF2 pass per candidate.

The salt also depends on the flash chip's unique ID. Re-imaging to a new ESP32-S3 invalidates every stored password — practical recovery story is "first-time setup wizard re-creates admin."

### 3.3 Brute-force coverage gap

Only the HTTP login + HTTP Basic Auth fallback are rate-limited. Serial, OLED, BLE, ESP-NOW (both bond-token and user:pass) accept unlimited attempts. The ESP-NOW user:pass path is the most exposed: a sender knowing the network channel can spray `user:pass:cmd` frames at full radio throughput with no consequence on this device.

The brute-force layer is also web-only conceptually: the keying is `LoginAttemptEntry.ip[40]` — there's no analogous "peer MAC" or "BLE conn id" key. Extending the same data structure to non-IP keys would mean changing the slot type.

### 3.4 Session-revoke vs identity-generation: clean split

The two protocols are deliberately decoupled (`System_AuthIdentity.h:146-167`):

| Event | Bump clock? | Revoke sessions? |
|---|---|---|
| BLE pair stamp | yes | no |
| user.add / approve | yes | no |
| user.promote | yes | no |
| user.demote | yes | yes |
| user.delete | yes | yes |
| user.changepassword (self) | no | yes (except calling session) |
| user.resetpassword (admin) | no | yes |
| login / logout | no | n/a |

This is clean — every mutator picks each axis independently. The cmd-handler header comments at `System_User.h:160-179` even tag each command with which protocols fire.

### 3.5 Session boot-ID

See §2.10. Single uint64-ish string, embedded at session create, checked at every authenticated request, set once at boot from eFuseMac + millis. No issues found.

### 3.6 Audit logging

Two separate log files:
- `/system/sys_logs/successful_login.log` (LOG_OK_FILE) — login successes only (logAuthAttempt filters non-login paths at `WebServer_Server.cpp:1512`).
- `/system/sys_logs/failed_login.log` (LOG_FAIL_FILE) — login failures only.

Command audit is separate (`logCommandExecution` → command-audit logs handled elsewhere). The login files are appended via `appendLineWithCap(... LOG_CAP_BYTES)` so they self-rotate, and `getTotalFailedLoginCount()` increments independently to give the OLED stats card a since-boot total.

---

## 4. Inconsistencies / smells

| # | Location | Issue |
|---|---|---|
| 1 | `System_User.cpp:1691` | `cmd_user_changepassword` always uses `getTransportUser(SOURCE_LOCAL_DISPLAY)` regardless of caller transport. Only correct when invoked from OLED. Web/serial/BLE callers should use `currentExecUser()`. |
| 2 | `WebServer_Server.cpp:2418-2489` (web) vs `System_User.cpp:1666-1720` (CLI) | Two parallel password-change implementations. Web validates and writes but **never revokes other sessions**. CLI revokes but with the wrong user (see #1). They have drifted. |
| 3 | `WebServer_Server.cpp:297` vs `:425` | Set-Cookie at session create lacks `HttpOnly` / `Secure` / `SameSite`. Clear-Cookie on logout *has* them. Asymmetric. |
| 4 | `HardwareOne.cpp:1700-1731` | Serial CLI inlines its own login flow rather than calling `cmd_login`. No rate limit, no `logAuthAttempt` for failures, no notification on success/failure path mirroring. |
| 5 | `System_User.cpp:2016-2096` (`cmd_session_revoke`) | Duplicates the loop body of `revokeUserSessions` instead of calling it. Two implementations of the same fan-out. |
| 6 | `System_User.cpp:2328-2355` + `WebServer_Server.cpp:1180-1185` | `loadUsersFromFile` reads a `passwordHash` field that no longer exists in users.json. Returns false. `gAuthUser="admin"`/`gAuthPass="admin"` stays at defaults; `rebuildExpectedAuthHeader` produces `Basic YWRtaW46YWRtaW4=`. The Basic-Auth fast path in `decodeBasicAuth` then admits any request whose `Authorization` header matches that string and downstream `isValidUser("admin","admin")` decides whether to accept — i.e. the fast path is mostly dead, but does it match a real "admin/admin" user, the fast path becomes a credential bypass disguised as an optimization. |
| 7 | `System_ESPNow.cpp:3754` | ESP-NOW `user:pass:cmd` path has no rate limit and no audit log. Adversary on the channel can offline-brute. |
| 8 | `Bluetooth.cpp:584-602` | BLE `login` defers to `cmd_exec_task` to avoid BTC stack blowup, but the per-conn login is unrate-limited. A misbehaving peer can hammer login attempts. |
| 9 | `System_User.cpp:156-194` | `isAdminUser` parses JSON with manual `indexOf` walks. Two paths to admin (explicit `role=admin` or "first user without role"). Demoting the first user can leave them as admin via the fallback if no other admin exists. |
| 10 | `System_User.cpp:402-437` | PBKDF2 salt is per-device, not per-user. Two users with the same password produce identical hashes. |
| 11 | `WebServer_Server.h:34` (`MAX_SESSIONS=2`) | Two-session-total cap is *fine* for a single-admin device but means a second admin user logging in evicts the first. The cap is per-user implicitly via the eviction loop, which means the device cannot serve two concurrent users at all. |
| 12 | `WebServer_Utils.cpp:267` (`COOKIE_BUF_SIZE = 512`, static) | Per-request cookie scratch buffer is a process-global static — not thread-safe across concurrent httpd workers. ESP-IDF's httpd runs handlers serially by default, but if `CONFIG_HTTPD_*` is ever raised, this becomes a race. |
| 13 | `WebServer_Server.cpp:3576-3619` (`handleLoginSetSession`) | Dead-looking handler that reads a global `gSessUser` and is only registered as a backup login-step. Renders JS that re-checks cookies and redirects. Doesn't appear to be reached in the main flow — `handleLogin` already calls `setSession` and 303s directly. Worth confirming whether this is still needed. |
| 14 | `tgRequireAuth` for SOURCE_BLUETOOTH / ESPNOW / MQTT | Returns true unconditionally. Comment says "Internal/ESP-NOW commands - already authenticated upstream" — but the upstream check is per-transport, not all transports have one. MQTT in particular has no analog. |
| 15 | `System_User.cpp:289` (`revokeUserSessions`) | Uses `username.equalsIgnoreCase` on all transports for matching, but `isValidUser` is case-sensitive at `System_User.cpp:849`. A user "Bob" could log in but never be revoked if revoke args are lowercased somewhere (or vice versa). |

---

## 5. Downsides and shortcomings

1. **Brute-force coverage is web-only.** Every other transport (serial, OLED, BLE, ESP-NOW user:pass) is unrate-limited. The "you need physical access" argument applies to serial+OLED but not BLE or ESP-NOW.
2. **Password-change is two diverged implementations.** Web POST and CLI command both call `setUserPassword` but have different post-conditions (session revocation), different user resolution (`ctx.user` vs `getTransportUser(LOCAL_DISPLAY)`), and only one (CLI) is even correct for non-OLED transports.
3. **Per-device salt** means rainbow tables work across users on the same device, and any device migration is a credential reset. Per-user random salt is cheap (16 bytes per user in users.json) and standard.
4. **`gAuthUser`/`gAuthPass` are vestigial.** They survive from a single-admin era. Either remove the Basic-Auth fast path entirely, or repurpose it to cache the most recently authenticated user's hash for O(1) re-auth — but the current shape is confusing dead code.
5. **`MAX_SESSIONS=2` is restrictive for a multi-user device.** Allowing user A and user B to be logged in simultaneously from different IPs is currently impossible — the second login evicts the first regardless of user.
6. **Cookie attributes are inconsistent.** Set without `HttpOnly`/`SameSite`/`Secure`; clear with all three. Anyone reading the source assumes the set is "intentionally permissive" — but the clear path tells the opposite story.
7. **Several auth surfaces inline-implement what `cmd_login` does** (serial CLI loop, web login handler) rather than calling a shared core. Three places need to agree on policy (rate limit, audit log, success notification, user-must-change-password redirect).
8. **`isAdminUser` JSON parsing is fragile** — manual `indexOf` is sensitive to JSON formatting, and the "first user without role is admin" fallback is undocumented at the call site.
9. **`cmd_session_revoke` and `revokeUserSessions` are duplicated.** Two implementations of the same logic; only one calls `bleRevokeUserSessions` correctly, and only one stores logout reasons correctly. Should be one function.
10. **ESP-NOW bond token never rotates** — set once on bond connect and used until passphrase changes or peer disconnects. A captured packet with the token replays cleanly unless the receiver dedupes by msgId (it does for some types but the auth check itself isn't replay-resistant). Adding a nonce/timestamp inside the signed body would help.
11. **The audit-log filter is path-substring based** (`cleanPath.indexOf("/login") >= 0`) — a route called `/configure-login-page` would get login-audited too. Use exact path match or transport tagging.
12. **No backpressure on session revocation.** Repeated `cmd_session_revoke user X` on the same user will keep stuffing the same 30-second-grace `revoked` flag, but the SSE notice ring is fixed at 2 entries (`NOTICE_QUEUE_SIZE=2` at `WebServer_Server.h:48`) — admin-side hammering can lose user-visible reasons.

---

## 6. Toward more coherent auth

### High-confidence cleanups

1. **Make `cmd_user_changepassword` use `currentExecUser()`** instead of `getTransportUser(SOURCE_LOCAL_DISPLAY)` (`System_User.cpp:1691`). Single-line fix.
2. **Have web `handlePasswordChangePage` POST call `cmd_user_changepassword`** (via the unified executor with the web AuthContext) instead of re-implementing. Eliminates the missing-revoke bug and keeps the two flows in sync forever.
3. **Delete `loadUsersFromFile` / `gAuthUser` / `gAuthPass` / `rebuildExpectedAuthHeader` / Basic-Auth fast-path** — the `passwordHash` field doesn't exist anymore. If Migration Tool needs Basic Auth, it already works via the slow path that calls `isValidUser`.
4. **Have `cmd_session_revoke user <u>` call `revokeUserSessions(u, reason)`** rather than re-implementing the fan-out (`System_User.cpp:2050-2096`).
5. **Make Set-Cookie symmetric with Clear-Cookie.** Add `HttpOnly; SameSite=Strict` to the session-create cookie (`WebServer_Server.cpp:245, 297`). `Secure` only when `gServerIsHttps`.
6. **Inline serial CLI login** (`HardwareOne.cpp:1700-1731`) → submit a `Command` with `transport=SOURCE_SERIAL` and let `cmd_login` handle it. Picks up audit logging and the success notification path for free.
7. **Audit-filter on exact path match** (`WebServer_Server.cpp:1512`) instead of substring `indexOf`.
8. **Tighten `tgRequireAuth` for SOURCE_BLUETOOTH / ESPNOW / MQTT** — at minimum, log a once-per-boot WARN when a command arrives without an upstream auth check so we know if any transport is propagating identity wrong.

### Design questions (worth a chat)

1. **Should brute-force lockout be transport-agnostic?** Adding BLE conn-id and ESP-NOW peer-MAC as alternate keys for `LoginAttemptEntry` would close the gap. Tradeoff: more memory for the slot pool, and the eviction policy gets harder (an attacker on multiple transports can't crowd out a legit IP if we shard by transport).
2. **Per-user salts?** Adds 16 bytes per user to `users.json` (or per-user settings). Standard practice. Migration: re-hash on next successful login under the old salt.
3. **Raise `MAX_SESSIONS`** to e.g. 4 to allow two distinct users simultaneously. Cost: ~1KB PSRAM per slot (the SSE rings dominate).
4. **Consolidate `tgRequireAuth` vs `isAuthed`?** Could become `bool requireAuth(AuthContext& ctx, ResponseMode mode)` where `mode` is one of `kSendChallengeOnFail / kNoResponseOnFail / kReturnJsonOnFail`. The three real call shapes (default 303 redirect, handleNotice JSON, handleLogin GET silent) collapse into one signature.
5. **Fix the dual-path admin determination.** `isAdminUser` should consult only `role=admin` (or whatever field is canonical) and never fall back to "first user." If "first user" needs special handling at bootstrap, do it explicitly in `adminCreateUser` by setting `role=admin` on the first user there.
6. **Should `OLED_Mode_ChangePassword` move its inline UI logic into the same back-end as `handlePasswordChangePage`?** It already does (it submits `userchangepassword` through `executeOLEDCommand`). What's needed is for *web* to do the same so all three UIs are siblings of one command.
7. **Replay-resistant ESP-NOW bond auth?** Right now the token is static for the bond lifetime. Adding a low-bit `nonce` field (timestamp truncated to minutes) inside the signed body, and recomputing the SHA on each frame, would make captured frames unreplayable past a few minutes. Modest CPU cost, no protocol break — bump the magic byte and gate on it.

---

## 7. Summary

The per-task TLS identity refactor is solidly landed: no `gExecAuthContext`/`gExecUser` relics, every cross-task hop installs identity via `ExecIdentityGuard`, every guarded VFS call resolves to the calling task's identity. The clock-vs-revoke split is unusually well documented and the cmd-handler annotations make new mutators easy to get right.

The pre-refactor smells that remain are concentrated in the **password-change path** (two diverged implementations, one with a misuse of `getTransportUser(LOCAL_DISPLAY)`), the **non-web auth surfaces** (serial inline, BLE/ESP-NOW unrate-limited), and **legacy single-user vestiges** (`gAuthUser`/`gAuthPass`, `loadUsersFromFile` reading a nonexistent field, `MAX_SESSIONS=2`). None of those are landmines today, but each is a tripwire when extending.

The most actionable single change is **routing the web POST `/account/password-change` through `cmd_user_changepassword`** (after fixing the latter to use `currentExecUser()` instead of the hard-coded LOCAL_DISPLAY user). That single move kills inconsistency #1 and #2 simultaneously and gives all four UIs (web, CLI, OLED, BLE) a single password-change implementation with consistent session-revocation semantics.

---

*Codebase review (May 2026). Scope: authentication and identity propagation across every transport.*
