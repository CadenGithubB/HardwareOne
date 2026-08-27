# Pre-1.0 Hardening Audit

**Date:** 2026-07-28
**Scope:** Whole-firmware pre-release hardening & readiness review — investigation only, no code changed.
**Method:** 11 parallel dimension audits, each independently re-verified by an adversarial second pass that re-opened the cited code. Findings below are the *verified* set; the verifier's adjusted severity is used throughout. One dimension (supply chain / build & release hygiene) was audited manually after its agent returned a placeholder.

---

## Executive summary

The security engineering in this codebase is well above typical hobby firmware, and the audit repeatedly confirmed that: `authorizeCommand()` really is a uniform chokepoint across serial, web, MQTT, voice, ESP-NOW, G2, OLED and automations; the ESP-NOW V4 session layer (SIGMA-I, X25519, ChaCha20-Poly1305, monotonic nonces, 64-frame replay window, constant-time HMAC compare) is cryptographically sound; the BLE Secure Channel is real forward-secret crypto, not obfuscation; the server-side path/permission layer resists traversal; there are no hardcoded secrets, no default credentials, no SoftAP/WPS/captive-portal exposure; and `.gitignore` correctly keeps build artifacts and real biometric capture data out of the repo.

The gaps cluster in four places. **First, the at-rest model is weaker than it looks:** every "encrypted" secret hangs off `getDeviceEncryptionKey()` = SHA256(eFuse MAC : flash UID : constant), which is reconstructible by anyone holding the device — and the crown-jewel secret, the home WiFi PSK, bypasses that layer entirely and is written *plaintext* to unencrypted NVS by the WiFi driver. **Second, the client-side web layer breaks the escaping discipline the server side maintains** — three separate stored-XSS paths (filename, ESP-NOW metadata, inline `.svg`) reach admin-privileged command execution, with no CSP to contain them, plus one unbounded allocation that gives any authenticated client a reliable remote reboot. **Third, the device does not automatically recover from a hang:** the Task Watchdog is subscribed by zero tasks with PANIC off, so a deadlocked task wedges the device with nothing to reboot it. (Commanded reboots and panic-reboots work correctly — this is specifically about hangs.) A related observation: `crashCount` is tracked but never read in a conditional, so nothing breaks a repeating crash cycle — though whether that *should* be automated is genuinely unresolved, see §5.2. **Fourth, privacy is under-served for a device that records audio, photos and GPS:** `factoryreset` preserves every capture and the WiFi PSK, captures default to the removable SD card, and any authenticated user can read them all.

**Top five before a public 1.0:** (1) one-line `esp_wifi_set_storage(WIFI_STORAGE_RAM)` to stop leaking the home WiFi PSK into plaintext NVS; (2) bound and null-check the `/api/cli` allocation — its sibling handler already does it right; (3) stop serving the filesystem as `SYSTEM` to any paired ESP-NOW peer (the "FTS AuthBypass" — this exfiltrates `users.json` and on-disk keys over RF); (4) HTML-escape the three XSS sinks and add baseline security headers; (5) make the Task Watchdog real, so a wedged task actually gets rebooted.

Two notes on what *isn't* a problem: the previously-tracked **MQTT bridge backlog item is fixed** (it authenticates per-message and routes through `authorizeCommand`; only a stale comment says otherwise), and **DeFlock is verified absent from code** (plan-only, nothing to strip). Secure Boot and Flash Encryption remain reasonable to defer — see "Deliberately lower priority".

---

## Priority actions before 1.0

| # | Area | Severity | Effort | Action |
|---|------|----------|--------|--------|
| 1 | WiFi creds at rest | High | Trivial | `esp_wifi_set_storage(WIFI_STORAGE_RAM)` at init; drop the PSK-printing debug line |
| 2 | Web DoS | High | Trivial | Bound + null-check `ps_alloc` in `handleCLICommand` (copy `handleCliBatch`) |
| 3 | ESP-NOW authz | High | Medium | Stop serving FS as `SYSTEM`; resolve a real principal, or opt-in + confirm |
| 4 | Web XSS | High | Small | HTML-escape filename / ESP-NOW metadata sinks; force `.svg` to download; add CSP + `nosniff` |
| 5 | Field reliability | High | Small–Med | Subscribe worker tasks to TWDT (or enable PANIC). Crash-loop handling is an **open question** — see §5.2 |
| 6 | BLE posture | High | Small | Ship the Secure Channel *effective* by default, or make status/UX reflect that it is inert |
| 7 | Password hashing | High | Small | Per-user random salt; raise iterations 10k → 100k (matches mesh/BLE on same HW) |
| 8 | Privacy / decommission | High | Small | Real `devicewipe` (incl. `/sd`); fix the capture-permission inversion |
| 9 | Secrets in PSRAM | High | Trivial | Move ESP-NOW session table + AES plaintext buffers to internal DRAM |
| 10 | Confused deputy | Medium | Small | Re-check local role before forwarding `remote:`/`@` commands |
| 11 | Release docs | Medium | Small | `SECURITY.md`, threat-model + privacy notice, third-party license notice |
| 12 | Dispatch primitive | Medium | Small | Make `executeCommandThroughRegistry()` internal-only or route it through auth |

---

## 1. Data-at-rest, boot & decommission

**Already solid:** bootloader watchdog on; ESP-NOW identity secret is at least AES-wrapped; `factoryreset` correctly uses system-auth for its delete; no hardcoded keys anywhere; the legacy `admin/admin` default was removed; `/api/settings` and `hwbackup` both redact passwords.

### 1.1 WiFi PSK written plaintext to unencrypted NVS — HIGH, trivial fix
The firmware AES-wraps WiFi passwords in `settings.json`, then hands the plaintext PSK to `esp_wifi_set_config(WIFI_IF_STA, …)` at [System_WiFi.cpp:959](components/hardwareone/System_WiFi.cpp:959) while driver storage is left at the IDF default `WIFI_STORAGE_FLASH`. Repo-wide grep for `esp_wifi_set_storage` / `WiFi.persistent` returns zero application hits — the Arduino core only calls `set_storage(RAM)` when `persistent==false`, and the bundled default is `true`. So SSID+PSK land in the `nvs.net80211` namespace in cleartext; `CONFIG_NVS_ENCRYPTION` is unset and `partitions.csv` gives `nvs` no encrypted flag.

**Risk:** `esptool read_flash` on a lost/stolen/resold wearable yields the victim's home WiFi password with commodity tooling and zero crypto work — fully bypassing the at-rest layer built to prevent exactly this. Corroborating evidence that it's unintended: [System_BootState.cpp:16](components/hardwareone/System_BootState.cpp:16) asserts NVS's "only contents are WiFi calibration and this boot state."

**Fix:** one call to `esp_wifi_set_storage(WIFI_STORAGE_RAM)` during init — the app already owns the persistent copy and re-applies it each boot, so nothing is lost. Also delete the cleartext PSK log at [System_WiFi.cpp:955](components/hardwareone/System_WiFi.cpp:955) (the analogous line ~878 was already commented out for exactly this reason; this one was missed). That log fans out to the web console ring even when the web output lane is off, because the web-mirror sink is gated only on buffer existence, not on `MSG_ROUTE_WEB`.

### 1.2 At-rest master key is reconstructible — HIGH
`deriveDeviceKeyFromIds()` = SHA256(`efuseMAC : flashUID : "HARDWAREONE_V1"`) at [System_Settings.cpp:267](components/hardwareone/System_Settings.cpp:267). The MAC is broadcast on every frame; the flash UID is an SPI `RDUID` command away for anyone holding the device. On read failure the code falls back to `flashUid = 0`, and `selectDeviceKeyEpoch()` [:1276](components/hardwareone/System_Settings.cpp:1276) *actively tries* that MAC-only candidate every boot. This one value derives the AES key, the PBKDF2 password salt, and the wrapper around the Ed25519 mesh identity.

**Nuance the verifier added:** the flash UID is *not* present in a flash data image, so a remote attacker with only a dumped image cannot trivially derive the key — the exposure is specifically the physical-possession threat, which for a wearable is the realistic one.

**Fix:** treat at-rest encryption as obfuscation and document it as such; at minimum refuse to derive a key when the flash-UID read fails, so a publicly-derivable epoch can never be adopted. Fix the password layer independently (§4.1) since that pays off regardless.

### 1.3 `factoryreset` is not a factory reset — MEDIUM
`factoryreset_confirmed` ([System_Utils.cpp:2181](components/hardwareone/System_Utils.cpp:2181)) deletes `users.json` and nothing else — no `nvs_flash_erase`, no LittleFS format, no per-user settings, no ESP-NOW identity, no captures. The prompt states verbatim that WiFi credentials are *preserved*. No wipe/format command exists anywhere in the tree.

**Risk:** a user who sells, gifts or discards the device reasonably believes "factory reset" cleaned it. It leaves the home PSK, mesh keys and every recording/photo/GPS track behind on unencrypted flash. Combined with §7.1 (captures default to a removable SD card that reset never touches), this is the sharpest privacy exposure in the audit.

**Fix:** rename current behavior (`resetaccounts`) and add a true `devicewipe` that formats LittleFS, calls `nvs_flash_erase`, and clears `/sd/recordings` + `/sd/photos`.

### 1.4 Single app slot, no update channel — MEDIUM
All five partition tables are factory-only (no `ota_0`/`ota_1`/`otadata`); zero `esp_ota`/`esp_https_ota` in the app; README documents `idf.py flash` as the sole update path. Absence of push-OTA is a defensible choice for a self-flashed device — **the security-relevant consequence is that there is no channel to ship a fix** for the RF/HTTP surfaces this device exposes. Note a dual-OTA layout does not fit the ESP-SR 16MB map (3MB model partition), so OTA would need a partition redesign — scope it explicitly rather than leaving it undefined. A "you are N releases behind" nudge is the cheap 1.0 answer (the device already knows its version and `SYSEVT_FIRMWARE_CHANGED` exists).

### 1.5 LittleFS auto-formats on first mount failure — LOW
[System_Filesystem.cpp:60-68](components/hardwareone/System_Filesystem.cpp:60) calls `LittleFS.format()` on the *first* mount failure with no retry grace. One transient/flaky mount destroys all user data. It does post `SYSEVT_STORAGE_FORMATTED`, so it isn't silent — but there's no confirmation and no backup. Gate behind a retry or one-boot grace.

---

## 2. Local web surface

**Already solid:** 96-bit `esp_random` session IDs, server-side only (no fixation); `HttpOnly` + `SameSite=Strict`; tiered per-IP lockout keyed on the real socket IP via `getpeername` (not spoofable `X-Forwarded-For`); boot-id invalidation; 60-min idle logout; single-session-per-user; no default credentials; server-rendered HTML consistently escaped via `streamHtmlEscaped`; bodies bounded on login/register/file-write/CLI-batch; traversal blocked by `normalizeFsPath` + role rules + sensitive-extension blocks. **A finder claim that logout fails to clear the cookie was refuted** — `clearSession` does emit the clearing cookie at [WebServer_Server.cpp:503](components/hardwareone/WebServer_Server.cpp:503).

### 2.1 Unbounded attacker-sized allocation → remote reboot — HIGH, trivial fix
[WebServer_Server.cpp:3194](components/hardwareone/WebServer_Server.cpp:3194): `ps_alloc(req->content_len + 1, …)` with **no size cap and no null check**, sized purely from the client's `Content-Length` header *before* any body is read. Send `Content-Length: 16000000` → `ps_alloc` returns NULL → `buf.get()[received] = '\0'` → StoreProhibited crash and reboot. The only throttle is a 50 ms *global* limiter.

The sibling `handleCliBatch` at [:5033](components/hardwareone/WebServer_Server.cpp:5033) already bounds at 32768 **and** null-checks. Copy that. This is the single cheapest high-severity fix in the audit.

### 2.2 Three stored-XSS paths, no CSP to contain them — HIGH
The server side escapes consistently; the client side does not.

- **Filename → `innerHTML`.** `rowHtml` concatenates the raw filename into markup ([WebServer_Utils.h:343](components/hardwareone/WebServer_Utils.h:343)) and assigns via `innerHTML` ([:527](components/hardwareone/WebServer_Utils.h:527)). `normalizeFsPath` rejects only `..`, double-quote and control chars — **not** `< > ' ( ) =`. The lone `esc()` helper is JS-string escaping for onclick handlers, never applied to the displayed name. A plain User (default catch-all grants `PERM_ALL`) can create `<img src=x onerror=…>.txt` in a shared path; it fires when an **admin merely browses the folder** — no click needed. Amplifier: the same renderer serves the ESP-NOW remote file explorer ([:1187](components/hardwareone/WebServer_Utils.h:1187)), so a *remote peer's* filename is injected unescaped too.
- **ESP-NOW metadata → `innerHTML`.** [WebPage_ESPNow.h:1817-1849](components/hardwareone/WebPage_ESPNow.h:1817) concatenates `deviceName`/`friendlyName`/`room`/`zone`/`tags` unescaped; ingest at [System_ESPNow.cpp:7452](components/hardwareone/System_ESPNow.cpp:7452) stores wire fields verbatim with length truncation only. `escHtml` exists at [:3226](components/hardwareone/WebPage_ESPNow.h:3226) and is used elsewhere — just not here. (Precondition: attacker must be an established session peer, so this is narrower than the filename path.)
- **`.svg` served inline.** [WebServer_Server.cpp:4384](components/hardwareone/WebServer_Server.cpp:4384) sets `image/svg+xml` with no `Content-Disposition`; `.svg` is absent from both `isImageFile()`'s edit-block and `hasSensitiveExtension()`, and View opens it as a top-level document — so `<script>` runs same-origin with the viewer's session.

All three reach **User → Admin escalation** (payload POSTs to `/api/cli` at admin privilege). And there are **no security headers anywhere** — grep for CSP / `X-Content-Type-Options` / `X-Frame-Options` returns zero hits tree-wide — so nothing contains them.

**Fix:** set the display name via `textContent` (or HTML-escape) in `rowHtml` and `renderExplorer`; escape the ESP-NOW metadata sinks with the existing `escHtml`; serve `.svg` as a download or add it to the sensitive-extension list; add a baseline CSP + `nosniff`.

### 2.3 Plaintext HTTP by default — HIGH (but expensive)
`CONFIG_ESP_HTTPS_SERVER_ENABLE` is unset. The HTTPS branch only runs if `httpsEnabled` **and** user-supplied certs exist at `/system/certs/`; any miss silently falls back to port 80 with `gServerIsHttps=false` ([WebServer_Server.cpp:5309](components/hardwareone/WebServer_Server.cpp:5309)), which also drops `;Secure` from the cookie. Realistically no enthusiast hand-installs a cert, so the shipped posture sends the admin password and session cookie in clear over WiFi. Full TLS on-device is a large lift with real RAM cost — **document the exposure prominently** rather than pretending it's covered, and consider making the silent HTTP fallback loud.

### 2.4 Lower-severity web items
- **Basic-Auth fallback re-sends the password every request** ([:552-571](components/hardwareone/WebServer_Server.cpp:552)), creating no session — widens the sniff window from once-at-login to every call. LOW.
- **Session tokens logged in full** at [:322](components/hardwareone/WebServer_Server.cpp:322)/[:376](components/hardwareone/WebServer_Server.cpp:376) via a macro gated only on route bits, not a debug flag. Default is serial-only, but enabling file logging persists live SIDs to a web-downloadable file. `authSuccessUnified` already truncates to 8 chars — apply that pattern. LOW.
- **Weakest password policy on the most-privileged account:** first-boot owner is gated only on `length() == 0` ([System_FirstTimeSetup.cpp:343](components/hardwareone/System_FirstTimeSetup.cpp:343)), while ordinary registration enforces `isValidPublicPassword`. A 1-char superadmin password is accepted. LOW, trivial.
- **Registration password is unvalidated for charset** and `snprintf`'d into a command line ([:3833](components/hardwareone/WebServer_Server.cpp:3833)) — no injection today (single-line executor) but passwords containing spaces mis-tokenize. LOW.
- No `Host`/`Origin` validation (CSRF rests solely on `SameSite=Strict`) and one wildcard CORS header at [:3393](components/hardwareone/WebServer_Server.cpp:3393) — confirm it's on a genuinely public endpoint. INFO.

---

## 3. Wireless / RF

**Already solid (ESP-NOW):** the unicast command core is genuinely well built — sound nonce construction, correct 64-frame replay window, full-length HMACs with constant-time compare, commands rejected outright unless session-encrypted, and ESP-NOW commands *do* pass `authorizeCommand`. No baked-in default passphrase. **Already solid (WiFi):** no SoftAP provisioning, no captive portal, no WPS, no open-AP fallback; the only AP is ESP-NOW's channel-parking AP with `ssid_hidden=1` and `max_connection=0`.

### 3.1 ESP-NOW session keys and the bond token live in PSRAM — HIGH, trivial fix
`gSessions` is `ps_alloc`'d with `PreferPSRAM` ([System_ESPNow_Sessions.cpp:26](components/hardwareone/System_ESPNow_Sessions.cpp:26)), and `SessionState` holds `aeadKeyTx[32]`, `aeadKeyRx[32]`, `aeadKeyRxPrev[32]`, `rekeyEphPrivKey[32]` and `bondToken[16]` inline — so the whole live key table sits in externally-probeable PSRAM. `gPending` ([:414](components/hardwareone/System_ESPNow_Sessions.cpp:414)) additionally holds queued command plaintext *including credentials* pre-encryption. This directly violates the project's own "never put secrets in PSRAM" rule, and it's inconsistent with `gIdentity` and `gMeshDerivedKeys`, which correctly live in static BSS. A valid bond token yields bond-admin identity, so this is the RCE credential. **Fix:** internal DRAM. Same for the AES plaintext buffers at [System_Settings.cpp:364](components/hardwareone/System_Settings.cpp:364)/[:490](components/hardwareone/System_Settings.cpp:490).

### 3.2 BLE ships effectively plaintext — HIGH
No link-layer security by design (no pairing/bonding/passkey; GATT chars are plain WRITE/NOTIFY) — all confidentiality rests on the app-layer Secure Channel, whose crypto is genuinely sound. **But it's inert by default:** `bleScRequired()` requires `bleRequireSecureChannel && bleSecureChannelSecret.length() > 0`, and the secret ships empty while the flag ships true. So the AND is false out of the box and `login <user> <pass>` is parsed from a plaintext write. Worse, the status command reports **"REQUIRED"** in exactly this inert state ([Bluetooth.cpp:1615](components/hardwareone/Bluetooth.cpp:1615)) because it reads the flag rather than effective enforcement — a user checking status is told the link is protected when it is not. (The boot notice does warn loudly, which mitigates.) There's also a bootstrap trap: provisioning `blesecret` *over BLE* transmits the passphrase in the clear.

**Fix:** make the status string reflect effective state (`REQUIRED but INACTIVE — no passphrase set (PLAINTEXT)`); refuse `blesecret` changes from `ORIGIN_BLUETOOTH` while unencrypted.

### 3.3 G2 glasses link has no cryptographic peer authentication — HIGH
Peer selection is by advertised name and/or saved MAC ([G2_Glasses.cpp:1250-1290](components/hardwareone/G2_Glasses.cpp:1250)) with no encryption on the client link, and `g2HijackAuthContext()` executes tap/gesture events as the pairing user ([G2_HijackCmd.cpp:177](components/hardwareone/G2_HijackCmd.cpp:177)) — which is admin when the owner paired the glasses. MACs are spoofable and the tap→command layout is open source, so a spoofed peripheral drives privileged CLI over the air with no challenge.

### 3.4 Other RF findings
- **No login throttle on BLE** — `cmd_login` has zero failed-attempt tracking, and the only durable mitigation (MAC ban) is defeated by per-attempt MAC randomization. MEDIUM.
- **BROADCAST_AUTH has no anti-replay** beyond a 5 s dedup ring — a captured `TIME_SYNC` replayed >5 s later rolls the mesh clock unconditionally ([System_ESPNow.cpp:2831](components/hardwareone/System_ESPNow.cpp:2831)). Bounded (crypto uses `millis`, not this offset) but corrupts time-based automations and log timestamps. MEDIUM.
- **Mesh fingerprint is an unkeyed CRC16** of a guessable label. Flags-0 opcodes (TEXT/BOOT/HEARTBEAT/SESSION_*) run with no auth check, and `v4hSessionOpen` does `ps_alloc` + enqueue *before* any identity check — an unauthenticated amplification vector with no per-source rate limiting. The verifier found it's **worse than reported**: a frame with `meshFingerprint == 0` bypasses the gate entirely, so the attacker needn't even guess the label. MEDIUM.
- **Group key gives membership authenticity, not sender identity** — the `origin` MAC is attacker-fillable and never compared to `src_addr` for broadcasts. Inherent to symmetric group keys; **document as an accepted limitation.** MEDIUM.
- **PMF capable but not required; WPA3 not enforced** ([System_WiFi.cpp:944](components/hardwareone/System_WiFi.cpp:944)) — permits deauth/downgrade nuisance. LOW.
- **Static public BLE address + fixed device name** ([BLE_IDF.cpp:515](components/hardwareone/BLE_IDF.cpp:515)) = wearer trackability while advertising. MEDIUM.

---

## 4. Authorization & credentials

**Already solid:** the chokepoint is real and uniform; empty identity is never admin; reserved usernames blocked at creation; automation `createdBy` re-checked owner-or-admin on mutate; rank checks prevent granting a role above your own; auth-posture settings correctly `requiresSuperAdmin`; disabling a transport's require-auth yields a *non-admin* `AuthBypass` identity rather than root. **The MQTT backlog item is resolved** — it authenticates per-message via `isValidUser` and routes through `authorizeCommand` ([System_MQTT.cpp:470](components/hardwareone/System_MQTT.cpp:470),[:529](components/hardwareone/System_MQTT.cpp:529)); only the comment at [System_User.cpp:214](components/hardwareone/System_User.cpp:214) still claims otherwise, and that stale comment is itself a hazard (a maintainer trusting it could add a genuinely unauthenticated path).

### 4.1 ESP-NOW remote filesystem served as SYSTEM — HIGH (the "FTS AuthBypass")
`FS_LIST`/`FS_STAT`/`FS_GET` are registered with `REQ_PAIRED | REQ_SESSION_ENC` only — **no `REQ_AUTHENTICATED`** ([System_ESPNow.cpp:5007](components/hardwareone/System_ESPNow.cpp:5007)) — and the handlers install `SYSTEM_IDENTITY_SCOPE` ([System_ESPNow_FsList.cpp:641](components/hardwareone/System_ESPNow_FsList.cpp:641),[:784](components/hardwareone/System_ESPNow_FsList.cpp:784),[:853](components/hardwareone/System_ESPNow_FsList.cpp:853)). `FsRole::SYSTEM` is *unrestricted* and exempt from the `hideAdminPaths` rules that constrain even a logged-in admin — so this path is **strictly more permissive than an authenticated admin's `files` browse**. Grep across the FsList reply path finds no enable-gate, no credential, no confirmation.

**Risk:** a peer holding **no account on this device** can enumerate and pull every file as SYSTEM — including `/system/users.json` (PBKDF2 hashes) and on-disk mesh/device keys. With flash encryption off, those on-disk secrets *are* the real keys. Note inbound file *writes* prompt for on-device confirmation; reads do not. Pairing now asks first, which raises the bar, but any peer ever accepted keeps root-equivalent read forever.

**Fix:** carry a credential and serve under that user's `FsRole` (never SYSTEM), and/or add an `espnowFsServe` opt-in defaulting off plus first-read confirmation.

### 4.2 `remote:`/`@` forwarding skips the local role check — MEDIUM
`authorizeCommand` runs on the **full raw line** at [System_Utils.cpp:4481](components/hardwareone/System_Utils.cpp:4481), *before* the prefix is stripped at [:4499](components/hardwareone/System_Utils.cpp:4499). `findCommand()` can't resolve a leading `@`/`remote:`, so `commandRequiresAdmin`/`SuperAdmin` both return false and the line sails through. Guests are blocked, but a plain User or non-super admin passes — and on the peer it executes as `kBondAdminUser`, which is treated as **super**. The comment at [:4530](components/hardwareone/System_Utils.cpp:4530) even states the caller's role is not re-checked. So `@factoryreset` from a low-privileged local principal runs as super on the peer: a textbook confused deputy. **Fix:** authorize the *unwrapped* inner command against the local identity, or gate forwarding behind super.

### 4.3 `executeCommandThroughRegistry()` bypasses authorization — MEDIUM (latent)
[System_Command.cpp:191-288](components/hardwareone/System_Command.cpp:191) invokes `found->handler()` directly with no `AuthContext` and no `authorizeCommand` — it is *not* the chokepoint, yet it's exported publicly and named to look like the safe entry point. All current callers pass hard-coded strings (no live exploit), but one of them is a web handler, and a single future caller passing attacker-influenced text yields role-free execution of any registered command. **Fix:** rename/privatize, or route it through auth.

### 4.4 Password hashing — HIGH
PBKDF2-HMAC-SHA256, but `salt = getDeviceEncryptionKey()` ([System_User.cpp:707](components/hardwareone/System_User.cpp:707)) — **device-wide, not per-user, and derivable** (§1.2). Two accounts with the same password produce byte-identical hashes, visible directly in `users.json`. And iterations are **10,000** ([:708](components/hardwareone/System_User.cpp:708)) while the *same codebase on the same hardware* uses **100,000** for mesh keys and BLE — 10× weaker for the highest-value secret, for no performance reason (HW SHA makes 100k ≈ 1 s). House style means no migration cost. **Fix:** per-user random salt + raise iterations. Also swap the `String ==` compare at [:751](components/hardwareone/System_User.cpp:751) for a constant-time one — low real risk (both sides are already PBKDF2 outputs, so there's no steerable oracle) but inconsistent with the deliberate `ctMemcmp32` used elsewhere.

### 4.5 Authorization granularity is per registered prefix, not per subcommand — LOW
`commandRequiresAdmin` reads a single `CommandEntry` flag ([System_Utils.cpp:3243](components/hardwareone/System_Utils.cpp:3243)), so one entry that internally dispatches both read and mutate subcommands carries one flag for all of them. The codebase compensates by registering separate longest-prefix entries — discipline, not a structural guarantee. Worth a targeted sweep of multi-subcommand handlers.

---

## 5. Robustness & recovery

**Already solid:** brownout at max sensitivity with flash-write protection; INT_WDT for interrupts-off hangs; panic→reboot not halt; NULL-returning allocator; orphan-tmp recovery; reset-reason/crashCount telemetry; and `writeTextAtomic` explicitly *refuses* a truncate fallback — the credential store is genuinely safe.

### 5.1 Task Watchdog is inert for the common hang — HIGH
`CONFIG_ESP_TASK_WDT_PANIC` is unset, only the idle tasks are subscribed, and repo-wide grep for `esp_task_wdt_add`/`esp_task_wdt_reset` returns **zero application matches**. `CONFIG_AUTOSTART_ARDUINO` is off so `enableLoopWDT()` is never called either. Net: a mutex deadlock or infinite loop in *any* worker task (LLM, ESP-NOW, HTTP, cmd_exec) — the common real-world failure — hangs the device forever with no automatic reboot, and even a monitored trip would only print a warning.

### 5.2 Nothing acts on the crash counter — OBSERVATION (remedy unresolved)

**The verified part.** `rtcCrashCount` is incremented on WDT/PANIC/BROWNOUT ([HardwareOne.cpp:1284](components/hardwareone/HardwareOne.cpp:1284)) and is **only ever assigned, displayed, or logged** — every usage is at lines 1231, 1286, 1290, 1293, 1380, 1401, 1424, and none of them is a conditional. ~14 subsystems autostart at [:1487-2122](components/hardwareone/HardwareOne.cpp:1487) and panic reboot delay is 0 s, so a crash that recurs on every boot produces an unbroken cycle escapable only by USB. That much is fact.

**What is *not* established:** whether this is worth fixing, and if so how. Treat everything below as an open question, not a recommendation. It has not been validated against any field data, and a misfiring breaker is worse than no breaker — it would disable working features on a healthy device.

**Also note the counter does not currently mean what a breaker would need.** It zeroes only on cold boot and `ESP_RST_POWERON` ([:1286](components/hardwareone/HardwareOne.cpp:1286),[:1290](components/hardwareone/HardwareOne.cpp:1290)), so it measures *crashes since the last power cycle*, not *consecutive* crashes. A device that crashed five separate times across weeks of healthy uptime carries a count of 5. Any threshold applied to it today would fire for the wrong reason.

#### Why a naive breaker is likely to misfire

These are the reasons to be cautious, not a design to implement around:

1. **Cumulative ≠ consecutive** (above). Would need a "boot deemed successful after N seconds" reset that does not exist.
2. **Brownout is not a software fault.** `ESP_RST_BROWNOUT` is in the increment set. On a LiPo wearable, a sagging battery or a WiFi-TX current spike can trip it repeatedly — disabling subsystems is arguably the wrong response to a power problem.
3. **A crash can be user- or attacker-induced.** Any command that panics increments the count (see §2.1). A breaker turns a transient DoS into a *persistent* feature-disable, which makes the failure mode worse, not better.
4. **Attribution is unreliable in this architecture.** Autostarts dispatch through the command bus (`runUnifiedSystemCommand("llmload …")`, `"openmqtt"`) and the real work runs on `cmd_exec_task` or a subsystem task. A per-autostart breadcrumb would only cover the synchronous submit window; a task crashing later, after several more autostarts have run, would leave the breadcrumb cleared or **pointing at the wrong subsystem**. This is the strongest argument that a per-feature breaker is not currently feasible.
5. **Memory corruption blames the wrong culprit** even when timing cooperates — subsystem A scribbles, subsystem B dies.
6. **The escape hatch defeats the detection.** A user who pulls the battery to break a loop zeroes the counter, so the device forgets it was ever looping.
7. **Safe mode is untested code that runs only in the worst moment.** A bug in a rarely-exercised recovery path surfaces exactly when the device is already broken.

#### A more defensible first step

If anything is done here before 1.0, **instrumentation is lower-risk than automation**: persist a short crash history (reset reason + uptime-before-crash + firmware version) so that when a user reports a problem there is evidence, and so a threshold could later be chosen from real data rather than guessed. That changes no boot behavior and cannot misfire. Whether a breaker is warranted at all is a decision to make *after* there is field data — and possibly the answer is that a USB reflash is an acceptable recovery story for a self-flashed device, which would make this a documentation item rather than a code change.

**Related:** this interacts with §5.1 and the live-assert config (`ASSERTIONS_ENABLE=y`, level 2, -O2) — an assert reachable at boot is the most likely way to produce a genuine loop.

### 5.3 Other reliability findings
- **No core dump** — `ESP_COREDUMP_ENABLE_TO_NONE=y` and no coredump partition exists in *any* of the five CSVs, so enabling it needs a repartition. Field crashes yield a one-word reset reason and no backtrace. MEDIUM for pre-1.0 triage.
- **Auto power-save is dead code, not just unfinished** — `checkAutoPowerMode()` ([System_Power.cpp:143](components/hardwareone/System_Power.cpp:143)) has its battery branch commented out **and zero callers anywhere**. Meanwhile its "TODO: get battery from hardware" is stale — `getBatteryPercentage()` already returns live data and is consumed in three other places. `SYSEVT_BATTERY_CRITICAL` has no load-shed/deep-sleep consumer. Yet `power auto on` prints "Will switch to PowerSaver when battery < N%" — a false promise. MEDIUM: either wire it up or remove the setting and the message.
- **`settings.json` is the only persist path not using `writeTextAtomic`** — [System_Settings.cpp:1097](components/hardwareone/System_Settings.cpp:1097) hand-rolls a writer whose rename-failure fallback opens the file `"w"` (truncate) and writes in place, reintroducing exactly the torn-write class the shared helper was written to eliminate. Mechanical one-line fix. LOW.
- **Stack checking is NORM only** (canary on return), and the memory monitor is warn-only — compounds §5.2. LOW.

---

## 6. Supply chain, build & release hygiene

*(Audited manually — this dimension's agent returned a placeholder.)*

**Already solid:** `.gitignore` is careful and well-commented — `build/`, `*.elf`, `*.bin`, `size.json` excluded, and `docs/HealthCapture/`/`docs/NewCapture/` excluded with an explicit note that they hold real biometric readings and usernames. `third_party/adarkroom` is gitignored (MPL-2.0 upstream). `dependencies.lock` **is tracked** and pins exact versions with component hashes, all from `components.espressif.com`. No hardcoded API keys, private keys, or default credentials anywhere. Serial CLI requires auth by default (`serialRequireAuth(true)`), as do BLE and the local display. The 41 tracked binaries are vendored Adafruit example bitmaps, not leaked artifacts. `isSecret` is correctly applied to G2 text-entry for PSK, new-user password, and login.

### 6.1 No `SECURITY.md`, no threat model, no privacy notice — MEDIUM
There is no `SECURITY.md`, `CONTRIBUTING.md`, `.github/` directory, or CI workflow, and `README.md` (174 lines) contains **no security, privacy, threat-model, warning, or disclaimer section at all** (grep returns zero hits). For a public 1.0 of a device that records audio/photos/GPS and exposes WiFi/BLE/ESP-NOW surfaces, this is the largest *documentation* gap: users get no responsible-disclosure channel and no statement of what the device does and does not protect. Given how much of this audit resolves to "document the accepted limitation" (§1.2, §2.3, §3.4, §7), a short threat-model section pays for several findings at once.

### 6.2 License composition needs a notice — MEDIUM (not legal advice)
`LICENSE.md` is **PolyForm Noncommercial 1.0.0** (source-available, not OSI open-source) with a blanket `Copyright (c) 2026 CadenGithubB`, over a tree that vendors **70 third-party libraries** under mixed terms. Two specifics worth attention:

- **`Adafruit_NeoPixel` is LGPL-3 and *is* compiled in** (it's in `SRC_DIRS`). Static linking into firmware triggers LGPL relinking obligations; combining LGPL-3 with a noncommercial-only license is the kind of thing worth a deliberate decision rather than an accident.
- **Five GPL-3 libraries are redistributed but *not* built** — `SD`, `WebSockets`, `WebServer_ESP32_ENC`, `WebServer_ESP32_{SC_,}W5500`, `WebServer_WT32_ETH01`, plus LGPL `CircularBuffer` and `Arduino_LSM6DS3`. They're dead weight in `SRC_DIRS` terms; **deleting them removes the question entirely.**
- `Adafruit_seesaw_Library` (compiled) ships **no license file**, and 13 other vendored libs also lack one.

**Fix:** add a `THIRD_PARTY_NOTICES.md`, scope the top-level copyright to first-party code, and delete the unbuilt GPL libraries. I'm flagging composition, not giving legal advice.

### 6.3 Test scaffolding ships in the binary — LOW
`G2_Page_TestSuite.cpp` is listed unconditionally in `CMakeLists.txt:171` with no `if(CONFIG_…)` guard, so the test suite is compiled into every release image — flash cost plus a little extra reachable surface. Consider a build-time gate.

### 6.4 Dependency ranges are caret, lock is pinned — INFO
`idf_component.yml` uses `^2.0.0`/`^1.0.20` ranges; the committed lock pins exact hashes, so normal builds are reproducible. Just be aware that regenerating the lock silently accepts new minor/patch versions of libsodium, esp-sr, tflite-micro and esp32-camera.

---

## 7. Privacy

**Already solid:** no telemetry and no phone-home to external servers; network exfil paths are auth-gated; MQTT GPS publishing defaults off; **DeFlock is verified absent from code** (grep returns zero non-doc hits — it's plan-only, so the compile-time-strip concern is satisfied by total absence); photos *are* capped (`cameraMaxStoredImages` defaults to 100 — a finder claim of "unlimited" was refuted).

### 7.1 Captures default to the removable SD card — MEDIUM (verifier-found)
`micPrimaryRecordingsFolder()` returns `/sd/recordings` whenever the SD is writable ([System_Microphone.cpp:54](components/hardwareone/System_Microphone.cpp:54)), and photos land on `/sd` too ([System_ImageManager.cpp:78](components/hardwareone/System_ImageManager.cpp:78)). A card pops out and reads on any laptop — no esptool, no flash dump, no account. It also escapes `factoryreset` entirely. **Any "wipe my data" command must clear `/sd` too**, and the privacy notice must call out removable media explicitly.

### 7.2 Access-control inversion on captures — HIGH
`/recordings` and `/photos` match **no** `PathRule` and fall through to the catch-all `{nullptr, PERM_ALL, PERM_ALL, PERM_ALL}` ([System_Filesystem.cpp:1391](components/hardwareone/System_Filesystem.cpp:1391)) — full read/write/delete for **any authenticated user**. Meanwhile mundane `/logging_captures/` is admin-only (`userPerms = 0`). The sensitivity ranking is exactly backwards: mic audio and photos are the most sensitive data the device holds and the least protected. `/sd/recordings` and `/sd/photos` fall through as well (the `/sd` rule is `exactMatch=true`). **Fix:** explicit rules for the capture paths.

### 7.3 No per-user ownership of capture data — MEDIUM
`PathRule` has no owner/identity dimension — access is purely path-prefix + role, so there is no "mine vs yours" on a device with a four-tier user model. All captures go to shared directories. Structural, worth deciding deliberately before 1.0.

### 7.4 Cross-user identity leakage — MEDIUM
The notification center renders `Login: <user>`, `Login failed: <user>`, `Msg from <sender>` ([System_Notifications.cpp:651](components/hardwareone/System_Notifications.cpp:651)) on any shared OLED/G2 view, and the event sink is durable (`/system/sys_logs/events.log`). Username↔IP correlation is possible by timestamp (the IP rides a separate `SYSEVT_LOGIN_LOCKED` event, not the same line).

### 7.5 Admin-readable network credentials — LOW (verifier-found)
`/system/settings.json` grants `adminPerms = PERM_READ` ([System_Filesystem.cpp:1348](components/hardwareone/System_Filesystem.cpp:1348)), where saved-network and MQTT broker credentials persist. Any Admin — not just SuperAdmin — can read them. Decide whether that's intended.

### 7.6 Unbounded recording retention — LOW
Each clip is capped at 60 s but there's no cap on `.wav` *count* and no auto-cleanup — only manual `micdelete`.

---

## Deliberately lower priority / accept the risk

These are real but poor value for a self-flashed enthusiast device — listed so effort doesn't go here:

- **Secure Boot v2.** Meaningless without a signing/release infrastructure, and it makes user self-flashing painful. The device's whole distribution model is "build and flash it yourself." **Skip; document.**
- **Burning `DIS_DOWNLOAD_MODE` / disabling JTAG.** Irreversible burns that mostly hurt the owner (they need download mode to reflash, JTAG to debug) and buy little without flash encryption. **Skip; document that physical USB access ⇒ full compromise.**
- **Anti-rollback.** Correctly absent — it's meaningless without Secure Boot.
- **Flash Encryption.** The only thing that *actually* fixes §1.2, but it's high-friction for self-flashing and complicates development. A defensible 1.0 decision either way — but if it stays off, §1.2's honest framing ("obfuscation, not protection") belongs in the docs.
- **Non-constant-time password compare.** Both operands are PBKDF2 outputs, so there's no steerable timing oracle. Fix for consistency, not urgency.
- **DNS-rebinding / `Host` validation.** The session cookie is IP-origin-bound and `SameSite=Strict`, so the authenticated path is already blunted.
- **AES-128-CBC without AEAD at rest, and 128-bit key truncation.** Genuine crypto-hygiene inconsistencies, but the confidentiality of these blobs is already defeated by §1.2 — only worth fixing if the at-rest key model is ever redone.

---

## Audit provenance

- 11 dimensions × (1 finder + 1 adversarial verifier). 20 of 22 agent results were substantive; the supply-chain dimension was re-done manually (§6). The completeness-critic and synthesis agents hit a spend limit and were replaced by manual synthesis.
- The verifier pass materially changed conclusions: it **refuted** the "logout doesn't clear the cookie" claim, **refuted** "photos are unlimited", **downgraded** several findings (PSRAM plaintext, timing compare) and **upgraded** others (`.svg` XSS, `/api/cli` DoS), and **found new issues the finders missed** — the filename XSS (§2.2), the SD-card capture exposure (§7.1), the uncalled `checkAutoPowerMode` (§5.3), and the `meshFingerprint == 0` bypass (§3.4).
- Severities are the verifier's adjusted values. Every finding cites `file:line`; nothing here was accepted on a finder's word alone.
