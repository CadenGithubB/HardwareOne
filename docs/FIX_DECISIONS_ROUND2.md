# Fix Decisions — Round 2

Ten findings, each independently re-verified at HEAD (`ee87ea7` v0.99.7) by a specialist instructed to
prove it already-fixed first, then to kill it, then to attack its own proposed fix.

**Nothing below was compiled, flashed, or run.** Every claim is source-read at the working tree. Line
numbers were re-derived by the specialists (several had drifted from the original audit) and I spot-checked
seven of the load-bearing ones myself.

| # | Title | Verdict | One-line reason |
|---|---|---|---|
| **8** | `sensorlog start <path>` writes any path as `FsRole::SYSTEM` | **GO — CRITICAL** | Non-admin command rotates `/system/users/users.json` out of existence; next account is superadmin. Minutes, not hours. |
| **4** | Unauthenticated ESP-NOW metadata → unescaped `innerHTML` | **GO — HIGH** | `flags==0` dispatch rows let any radio write attacker strings that execute in an admin's browser session. Escaping fix is unambiguous; a second, optional part touches a recorded decision. |
| **7** | Guest-allowed `logout g2` re-homes the lens to the owner | **GO — HIGH** | Guest gate matches the verb only; `logout g2` clears the pair-stamp and re-stamps to the super-admin owner, persisted to flash. |
| **13** | `writeText()` returns `true` unconditionally | **GO — HIGH** | `writeTextAtomic` commits a truncated/empty `users.json` over a good one and reports success. Lockout or a re-claimable device. |
| **14** | Mesh passphrase decrypted before device-key epoch selection | **GO — HIGH** | Wrong-epoch decrypt yields empty passphrase; the mesh writer's preservation branch is structurally unreachable, so the next save destroys the recoverable blob permanently. |
| **12** | `handleSettingCommand()` discards `writeSettingsJson()`'s bool | **GO — HIGH** | Reports "Configuration updated" while the device logs "settings NOT persisted". Includes `serialrequireauth`. |
| **5** | ESP-NOW `FS_LIST/STAT/GET` serve the whole FS as SYSTEM | **DESIGN-CALL** | Documented in two places as deliberate ("a securely paired + session peer is trusted at device level"), but the same firmware's `espnowbrowse` demands credentials. Owner must pick the trust boundary. |
| **9** | G2 lens runs CLI as the paired user (= superadmin by default) | **DESIGN-CALL** | "Pairing IS auth" is written down as a decision in four places. The unstated consequences (SUPER tier; no BLE link security) need an owner answer, not a patch. |
| **6** | `blePushEvent` unbounded `snprintf` + unescaped `%s` | **NO-GO** | Fixed in `bda65a95` (v0.99.5). The audit read the pre-fix function. Fix re-attacked six ways and holds. |
| **11** | `@`/`remote:` wrapper defeats redaction | **NO-GO** | Refuted at the named sites — the unknown-verb catch-all added in v0.99.5 masks the whole tail. **But it turned up two real leaks elsewhere in the same feature — see below.** |

---

## 1. NO-GO — do not spend time here

### 6 — `blePushEvent` overflow / JSON injection: **already fixed, verified sound**

The audit was reading `bda65a95~1`. Its cited line range (2210-2233) lands exactly on the pre-fix
function, which really did have `char eventJson[256]`, a would-be-length accumulator, `size_t`-underflowing
`sizeof(buf) - pos`, and bare `%s` inside JSON string literals.

At HEAD: `char eventJson[512]`, a bounded `jsonEscapeInto()` (Bluetooth.cpp:2235) that also `\u`-escapes C0
controls, and every append bounds-checked *before* `pos` advances. There is a 20-line comment at
Bluetooth.cpp:2266 documenting the fix in past tense, naming the `requiresAdmin=false` reachability and the
underflow. `git log -S'jsonEscapeInto' -- Bluetooth.cpp` → exactly `bda65a95`.

The specialist did not stop at "it's fixed" — it attacked the shipped fix six ways: worst-case length
arithmetic (max ~344 of 512, guards fail closed), `size_t`/`int` mixing at every `sizeof - pos` site,
the escaper's `o + need >= dstCap` NUL-slot reservation, escape-expansion attacks (2× quotes, 6× controls,
all absorbed by the 192-byte intermediate because escaping precedes measurement), DoS on the BLE notify
path, and struct-layout impact. **The fix holds.** One cosmetic residual judged not worth filing: messages
over 191 escaped bytes are silently shortened while `cmd_bleevent` still reports success.

**Process note:** this audit run used a pre-v0.99.5 snapshot of `Bluetooth.cpp`. Any other candidate from
the same run touching that file should be re-based against HEAD before triage.

### 11 — `@`/`remote:` redaction bypass: **refuted at the named sites — but two real leaks fell out**

The claim was that wrapping a credential command (`@login bob hunter2`) escapes `redactCmdForAudit`. It
does not. `bda65a95` added an unknown-verb catch-all *after* the rule loop (System_Utils.cpp:1200):

```cpp
if (!findCommand(c)) { int sp = c.indexOf(' '); if (sp > 0) return c.substring(0, sp) + " ***"; }
```

`@login bob hunter2` → `"@login ***"`. `remote:login ...` → `"remote:login ***"`. `remote login ...` →
`"remote ***"`. The wrapper makes redaction *coarser*, not absent. The send-path echo at
System_Utils.cpp:4729 additionally carries a `// SECURITY:` comment naming this exact leak and redacts for
itself. The specialist checked the one way this could break — a registered command literally named
`remote` would make `findCommand` match and skip the catch-all — and confirmed no such command exists
(all `"remote` hits are OLED mode slugs, notification source strings, and CSS class names).

**Two genuinely open leaks found while verifying. Neither is this candidate; do not let them die with it.**

- **ESP-NOW receiver echoes the raw inner command, pre-auth.** System_ESPNow.cpp:5866 prints
  `actualCmd` verbatim *before* `isValidUser()` runs; twins at :5809 (bond) and :5956
  (`SYSEVT_REMOTE_CMD_RX`). Sink is `BROADCAST_PRINTF` → `gWebMirror` → `/api/logs`, whose only gate is
  `tgRequireAuth` + non-guest — **any plain standard-role user reads it.** This is already filed as
  docs/AUTH_SECURITY_REVIEW.md:401-407 [MEDIUM]; verification confirms it verbatim at HEAD. Track it there.
- **`espnowremote` tail is deliberately un-redacted.** `redactPeerCredCmd` (System_Utils.cpp:1066)
  masks only the peer password, by design, so `<rest>` stays audit-visible. Nobody considered a credential
  command nested in `<rest>`: `espnowremote B alice pw useradd bob s3cretpw 0 admin` writes the *inner*
  password to `/system/sys_logs/command-audit.log` and the web console feed. Same at
  System_ESPNow.cpp:14405. **This one does not appear to be filed anywhere.** Fix is a bounded recursion
  into the tail with an explicit "do not recurse into another peer-cred verb" guard (depth cap 1 — the
  unbounded version is a stack hazard on a 6.5 KB `espnow_task`).

Also noted while checking: `DEBUG_CMD_FLOWF` at System_Utils.cpp:4609 and WebServer_Server.cpp:3287 do
print the raw line, but `debugCommandFlow` defaults false (System_Settings.h:86). Not a live leak.

---

## 2. DESIGN-CALL — answer the question before anyone writes code

Both of these are real, compiled, and reachable. Both are also *written down as your decisions*. A patch
here changes the product, not a defect. **No fix is recommended for either — that is deliberate.**

### 5 — ESP-NOW remote filesystem serves everything as `FsRole::SYSTEM`

**Factually true.** Dispatch rows System_ESPNow.cpp:5008-5013 gate `FS_LIST/STAT/GET` on
`REQ_PAIRED | REQ_SESSION_ENC` and nothing else. All four responder sites install
`SYSTEM_IDENTITY_SCOPE(...)` (System_ESPNow_FsList.cpp:644, :784, :842, :896). `systemIdentity()` sets
`transport = SOURCE_INTERNAL; user = "system"` → `resolveRole` returns `FsRole::SYSTEM` →
`permsForRole` returns `rule.systemPerms`, which is `PERM_ALL` on **every row** of `sPathRules`, and
`isUnrestrictedRole(SYSTEM)` short-circuits the sensitive-extension guard entirely.

**Two corrections to how this was previously framed, both in your favour:**

- The prior audit's examples were wrong. A local ADMIN *can* read `/system/users/users.json`
  (`.json` is not a sensitive extension) and *can* read `/system/certs/*.key` (that rule sets
  `exemptSensitiveExt=true`). The claim "files a local admin cannot read" is false for both named files.
- The real SYSTEM-over-ADMIN delta is narrower: `/system/ota/` (admin gets literally zero, the peer gets
  `PERM_ALL`), and any sensitive-extension file outside an exempt directory (`/espnow/*.bin`,
  `/logging_captures/*.bin`, anything named `*secret*`/`*password*`).
- The precondition is also narrower than "anyone in radio range": `SESSION_OPEN` requires a stored
  peer identity **and** an Ed25519 signature verify over the handshake transcript. The attacker is
  "a device you ran `espnowpairsecure` with", not a MAC spoofer.

**What the author intended (verbatim, System_ESPNow_FsList.cpp:641-646):**

> Read under SYSTEM identity — a securely paired + session peer is trusted at device level (FsList is base
> ESP-NOW, not bond-gated). Per-user identity propagation can be layered on later via a token in the
> request's reserved bytes.

and System_ESPNow.cpp:5002, above the rows:

> Gated only by REQ_PAIRED + REQ_SESSION_ENC: any securely paired peer with a live session may
> enumerate/pull files (served under SYSTEM identity).

Both the flag set and the identity choice are named and justified. This is intent.

**The one fact neither comment addresses**, and the reason this needs your answer: the same firmware ships
a *second* remote-browse path that takes the opposite position. `cmd_espnow_browse`
(System_ESPNow.cpp:14215) sends `"<user>:<pass>:files \"<path>\""` and its help text says
"Credentials are verified ON THE TARGET device, not this one." So `espnowbrowse`/`espnowfetch` run under a
real target account's role — subject to path rules and the sensitive-extension guard — while the binary
FsList path carries no credentials and runs as SYSTEM. Nothing explains the asymmetry.

**THE QUESTION:** Is "a device I ran `espnowpairsecure` with == full-filesystem trust, no account needed"
the boundary you want? If yes, the credentialed CLI path needs a comment explaining why it exists at all.
If no, this is an open gap and the fix is the per-user token your own comment defers.

*(If you later want a middle option: `AuthContext::scope` already exists and is already enforced in
`checkPerm`; setting it from **local settings only** would confine the peer without touching role
semantics. It does not close the sensitive-extension gap inside the scope. Not a recommendation — noted so
you know the lever exists.)*

**One implementation trap if you ever do act:** there are **four** SYSTEM installs, not three. `:842` is
the GET size-probe/ACK and `:896` wraps the actual `sendFileToMac`. Tightening one without the other
produces an OK ACK followed by a denied read — and the comment at System_ESPNow_FsList.cpp:880 says that
state is unrecoverable: the requester's pending slot was already cleared by the ACK, `fsListTick`'s sweep
only covers a *lost* ACK, so the OLED sits on "Downloading..." forever. All four change together or none do.

### 9 — G2 lens executes CLI as the paired user

**Factually true.** `g2HijackAuthContext()` (G2_HijackCmd.cpp:177) sets `ctx.user =
gBlePeerData[BLE_PEER_G2_GLASSES].pairedByUser` with no credential of any kind; `tgRequireAuth`'s G2 branch
(System_User.cpp:195) checks only that the string is non-empty. It feeds both the async submit path and 17
`G2HijackCtxGuard` sites doing direct FS access. In the default single-owner deployment that user is the
superadmin owner — because first-time setup writes `role = "superadmin"` for account #1, and
`bleResolveStampUsername` falls back to `getDeviceOwnerUsername()` for anonymous boot-reconnect workers.

**One correction:** `FsRole::SUPER` is *derived*, not installed. If a plain user paired the lens it
resolves to `FsRole::USER`. It reaches SUPER in the default deployment, not by construction.

**Aggravating fact the original claim did not have** — and the reason this is not purely a
physical-possession threat model: **there is no BLE link-layer security anywhere in first-party code.**
Zero tree-wide hits for `BLESecurity`, `setSecurityAuth`, `ESP_GATT_PERM_*_ENC`, `ESP_LE_AUTH`,
`esp_ble_gap_set_security_param`. The lens is matched by an advertised-name regex
(G2_Glasses.cpp:1525) or a saved *public* MAC (:1614). Both are forgeable by an attacker-controlled ESP32
in radio range.

**What the author intended (four independent sites):**

- G2_HijackCmd.cpp:26 — "Persisting the username rather than synthesizing a fake 'g2_hijack' account means
  glasses inherit exactly the privileges of their owner... No synthetic auth bypass."
- BLE_Peers.cpp:208 — "The glasses have no credential login, so pair-time is when their owning user is
  captured (this is the login-equivalent for that transport)."
- BLE_Peers.cpp:167 — the owner fallback is deliberate: "home the peer to the device owner so
  mac+autoReconnect can never leave G2 (or other peers) permanently unowned."
- System_Filesystem.cpp:1580 — SUPER on every transport is deliberate: "Deliberate scope, chosen by the
  device owner: a superadmin has unrestricted filesystem access on EVERY transport. That includes over the
  air... Narrowing this to local transports means gating on `ctx.transport` here."

**docs/AUDIT_TRIAGE.md:172 is wrong** where it says "the silent escalation of pairing to SUPER rather than
ADMIN is not [deliberate]". The `resolveRole` comment above explicitly accepts super-on-every-transport as
an owner decision. All three legs carry intent comments, so the composite is your call.

**THE QUESTION:** You accept "pairing is auth". Do you also accept its two unstated consequences —
(a) a lens tap runs at SUPER, i.e. the lens Files browser reads `/system/certs/*.key` and other users'
PBKDF2 hashes, strictly *more* permissive than an authenticated web admin; and (b) the surface is not
physical-possession-limited, so anyone in BLE range who can spell `Even G2_00001_R_` gets the owner's CLI?

**Do not ship the previously-suggested fix.** docs/AUTH_SECURITY_REVIEW.md:1548 proposes removing the owner
fallback in `bleResolveStampUsername`. The specialist attacked it and it is a self-inflicted DoS: on a
headless boot the reconnect worker has no identity and no live session, so `pairedByUser` stays blank,
`g2SubmitHijackCommand` fail-closes on every tap, and every VFS read denies at `FsRole::ANON`. Recovery
requires reaching a serial/web console — **after every reboot**. That is precisely the failure the
BLE_Peers.cpp:167 comment exists to prevent.

**UNVERIFIED:** the specialist did not build or flash, and did not enumerate which specific lens commands
are `commandRequiresSuperAdmin`. If you cap the lens tier, the blast radius on the lens settings editor is
unquantified.

**Note the interaction with finding 7:** a guest can *escalate* the lens from a plain user to the
super-admin owner via `logout g2`. That half is a real defect regardless of how you answer this question,
and it is fixed independently below.

---

## 3. GO — real, unintentional, compiled, reachable

### 8 — `sensorlog start <path>` writes anywhere as SYSTEM — **CRITICAL**

**Defect.** `sensorlog` is registered `requiresAdmin=false` (System_SensorLogging.cpp:2454) and its only
destination validation is `filepath.charAt(0) != '/'`; every subsequent FS operation — recursive parent
mkdir, create, delete-on-header-failure, per-tick append, and rotation rename/remove — runs under
`VFS::systemAuth(...)`, i.e. `FsRole::SYSTEM` with `PERM_ALL` and an **empty scope**. A plain user runs
`sensorlog maxsize 10240` / `sensorlog rotations 0` / `sensorlog start /system/users/users.json 100`, waits
for ~10 KB of heartbeat rows, and the rotation branch calls `removeGuarded` on the live account database —
after which `detectFirstTimeSetupState` arms the wizard and the next account created is `superadmin`.

The author already fixed this exact hazard class in the **same file** for the sibling command
(System_SensorLogging.cpp:2232, `cmd_healthlogmerge`): *"SCOPED: the unscoped overload leaves ctx.scope
empty, pathWithinScope then returns true unconditionally, and permsForRole hands back rule.systemPerms."*
`capturecrypt export` got the same treatment at :2396. `sensorlog start` is the un-migrated holdout — and
the only one of the group that is `requiresAdmin=false`.

**Fix.** Two-tier admission gate inserted after `ensureDebugBuffer()` (System_SensorLogging.cpp:1202),
*before* the mkdir walk at :1204. Normalize first (so the gate sees exactly the string the guarded VFS
will), then: paths inside the capture tree are always allowed (this command's own output area — `userPerms`
is 0 there so the *file manager* doesn't hand it out, not because logging is privileged); any other literal
path is admitted only if `canEdit(path, currentAuthContext()) && canCreate(...)` — i.e. only if the caller
could have written it under their **own** identity. Blanket-confining to the capture tree would kill the
documented MANUAL literal-path mode; checking caller rights on *all* paths would lock plain users out of
their own default log path.

`normalizeFsPath`, `canEdit`, `canCreate`, `currentAuthContext`, `VFS::pathInCaptureTree` and
`VFS::Scopes::CAPTURES` are all already in scope via headers included at :22/:23/:25. ~20 lines, one file.

**Callers checked.** Every caller enumerated. In-tree and unaffected: OLED_Mode_Map.cpp:1541,
WebPage_Maps.cpp:135, `healthlogging` (:2094), G2_Glasses.cpp:5901, OLED_Mode_Logging.cpp:227,
`sensorLogAutoStart` (:2598), WebPage_Logging.h:645. Behaviour changes only for out-of-tree manual paths,
where it now matches what the caller could do via the file manager: a plain USER keeps `/mylogs/x.txt`
(catch-all row is `PERM_ALL`), USER and ADMIN are both refused under `/system/`, SUPER still passes.

**Self-attack.** (a) Verified the gate sees the *real* caller — `CommandIdentityScope` installs TLS before
dispatch, and neither G2 taps (run as the pairing user) nor OLED (`SOURCE_LOCAL_DISPLAY` or the reserved
`AuthBypass` name) can resolve to `FsRole::SYSTEM`, which requires `SOURCE_INTERNAL && user=="system"`.
(b) Fails **closed**: an empty identity → `FsRole::ANON` → denied. The two identity-less callers
(boot autostart, `i2csensor_pa1010d.cpp:731`) use the in-tree default path and take branch 1 — and a
`settings.json` already poisoned with an out-of-tree path is now refused at boot, which neuters the
persistence leg. (c) TOCTOU: `filepath` is later rewritten by `resolveSessionTarget`, rotation (`+".1"`)
and the SD overflow mirror — all three stay in the **same directory**, so the same `PathRule` governs.
(d) No struct touched. (e) Accepted residual: `hasSensitiveExtension` will now refuse an out-of-tree
`/mylogs/secret.txt` for a non-super caller. Arguably correct.

**UNVERIFIED:** whether appending rows *after* a complete JSON object makes `deserializeJson` error
(ArduinoJson version not pinned down). Only affects the "corrupts before rotation" accelerator; the
rename/delete leg is confirmed from code and is sufficient alone.

**Confirm it worked (hardware).** As a plain non-admin user:
`sensorlog start /system/users/users.json 100` must return `Error: not permitted to log to ...` (before the
fix it returns success and starts appending). Then confirm no regression: `sensorlog start` with the
default path still works, the boot autostart still grows `/logging_captures/sensors/sensors.txt`, and an
out-of-tree path the user *does* own (`/mylogs/x.txt`) still starts.

**Small open question when you apply it:** keep out-of-tree MANUAL paths gated on caller rights (what the
fix does), or hard-confine `sensorlog` to the capture tree like `healthlogmerge`? Simpler, but drops a
documented mode.

### 4 — Unauthenticated ESP-NOW metadata reaches admin `innerHTML` — **HIGH**

**Defect.** Three dispatch rows carry `flags == 0` (System_ESPNow.cpp:4987-4989) — no pairing, no session,
no HMAC — and `v4h_metadata_resp_push` copies `deviceName/friendlyName/room/zone/tags` verbatim into
`gMeshPeerMeta` with length truncation only, allocating a slot for **any** new MAC. Those strings are then
concatenated raw into `innerHTML` at five sites in WebPage_ESPNow.h — both the click path (`:1817-1849`,
Metadata tab) and the 10-second poll path (`:739`/`:780`) — executing in an authenticated admin's session,
where `/api/cli` runs at the viewer's privilege and there is no CSP anywhere (zero tree-wide hits for
`Content-Security-Policy`).

**Two corrections to the original framing:** (i) the stated precondition "attacker must be an established
session peer" is **wrong** — `flags==0` means none of that; (ii) the no-click poll variant additionally
requires mesh mode ON (defaults false), but the mesh-off variant still works by spoofing an
already-paired peer's src MAC (MACs are in the clear on every frame) and waiting for the admin to open that
peer's Metadata tab.

**PART 1 — escaping. Do this. Plain bug fix.** ~15 lines, one file.

The audit's own prescription ("use the existing `escHtml`") **would not compile-time fail but would
ReferenceError at runtime** — `escHtml` (WebPage_ESPNow.h:3226) is scoped inside the Chunk-5d IIFE and is
not global. `hw._esc` *is* global but is `textContent`→`innerHTML`, which does **not** escape `"` — unusable
for the attribute-context sink. So: define a local `escH` helper at the top of each of the two IIFEs
(Chunk-3B after `:604`, and the Chunk-5c IIFE containing `loadPeerMetadata`) escaping `& < > " '`, then
wrap four sites in `renderUnifiedDeviceList` (`:687`, `:739`, `:762`, `:769`) and the five metadata fields
at `:1817-1841`.

At `:769` the existing `.replace(/'/g, "\\'")` is dropped — `escH` already turns `'` into `&#39;`, which
cannot terminate either the JS string literal or the HTML attribute.

**PART 2 — the ingest hole. Owner's call, one line.** Set `V4_OPC_FLAG_REQ_SESSION_ENC` on the
`METADATA_RESP`/`METADATA_PUSH` rows. This is provably non-regressive: `sendMetadata`
(System_ESPNow.cpp:7423) sends both opcodes exclusively via `v4_send_encrypted_or_queue`, so every
legitimate frame of these types is *already* a session frame; the flag only drops forged plaintext. The
specialist deliberately did **not** add `REQ_PAIRED` — `isPaired` tests a different table from the
peer-identity table and lockstep was not verified.

**Why this is a question and not just a fix:** docs/ESPNOW_OPCODE_REORG_PLAN.md:155-157 records the
`flags=0` choice, but every recorded rationale is about **egress** ("ungated RX leaks nothing to unpaired
radios", "TX strict encrypt-or-queue"). Nothing addresses accepting inbound *writes* from an
unauthenticated radio. The same table explicitly labels other `flags=0` rows as seams ("CMD_RESP...
spoofing seam", "TEXT... chat from unpaired senders accepted") — so you do annotate RX-integrity seams when
you see them, and did not here. **Was ungated ingest ever an intended property, or did the decision only
ever consider egress?** If ingest was never meant to be open, apply Part 2. If it is wanted (learning
strangers' names pre-pairing), Part 1 alone ships and the opcode table should label these rows a seam.

*Secondary: `METADATA_PUSH` (55) is documented as never transmitted by any firmware path. Its RX plumbing
is pure attack surface. Retire the opcode instead of gating it?*

**Callers checked.** The escaping fix touches only two JS render functions. Critically, `deviceName` is also
stored raw into `window.__espnowDeviceNameToMac[deviceName]` and `window.espnowDevices.push({name: ...})`
at `:631-633`, which feed device selection and later CLI command construction — **escape only inside the
HTML concatenation, never the stored value.** The `meshBadge` at `:681` is already safe (server-validated
by `isValidMeshLabel`). Leave it alone. The OLED/G2/CLI consumers of `gMeshPeerMeta` render to non-HTML
surfaces and are untouched.

**Self-attack.** Rejected promoting `escHtml` to a global (creates a cross-chunk load-order race on the
first poll); checked each of the four sites for its context before picking the escaper (`hw._esc` fails the
attribute site); confirmed no double-escaping (`Kitchen & Bath` renders correctly); confirmed Part 2 cannot
wedge the deferred slot (the drop happens *before* the handler, so `deferredMetadataPending` is never set)
and in fact **removes** an existing DoS where an attacker iterates MACs to exhaust every `gMeshPeerMeta`
slot; no struct layout changed.

One accepted behaviour change: at `:769` the escaped name is passed to `pairUnpairedDevice`, which builds
an `espnowpair` CLI line — a discovered name containing a quote would be paired under an entity-encoded
name. Only hostile names contain those characters.

**Found in passing, deliberately NOT bundled:** `processMetadata`'s
`strncpy(meta->tags, metadata->tags, sizeof(meta->tags) - 1)` reads up to 63 bytes from a 54-byte source
field, pulling in `stationary`, `reserved[3]` and ~6 stale bytes. Stays inside the 216-byte backing store,
so not an OOB read — but it is a real field-boundary bug adjacent to this code.

**Confirm it worked (hardware, two devices — or hand-edit the stored metadata on one).** Set a peer's
`friendlyName` to `<img src=x onerror=alert(1)>`, open the ESP-NOW page as admin. Before: alert fires.
After: literal text renders. Then check a legitimate name containing `&` still displays correctly, and that
Pair on a discovered device still works.

### 7 — Guest `logout g2` re-homes the lens to the super-admin owner — **HIGH**

**Defect.** The guest gate inspects the **first token only** (`commandAllowedForGuest`,
System_Utils.cpp:4420 — verified verbatim), so `logout g2` passes; `cmd_logout` then calls
`logoutTransport(SOURCE_G2_GLASSES)`, which clears `pairedByUser` and *immediately* re-stamps it via
`bleStampPairedByIfBlank` → `bleResolveStampUsername`, which rejects the guest caller's own identity and
falls through to `getDeviceOwnerUsername()`. `setSetting` persists it to flash. Every subsequent lens tap
and lens-typed CLI line then runs under that name (G2_HijackCmd.cpp:180).

**Split the intent carefully.** The *clear-then-re-home* policy is deliberate and documented at three
sites (System_User.cpp:516, :605, :1069; BLE_Peers.cpp:167) — **do not "fix" that.** What is not
intentional is a guest being able to trigger it. Contrary intent at the sites: System_Utils.cpp:5421
("admins may want to clear pairedByUser") and BLE_Peers.cpp:136 ("This avoids surprise privilege swaps if a
non-admin briefly handles the device"). `logout g2` is an undocumented third way to "clear the peer first",
available to the lowest tier, producing exactly the swap that comment exists to prevent.

**Fix.** One early return in the `rest == "g2"` branch of `cmd_logout` (System_Utils.cpp:5420):
`if (!currentExecIsAdmin()) return "Error: Admin access required to log out the G2 lens";`
plus a comment correcting the now-stale claim at :5422 that recovery needs `bleautoreconnect g2-glasses on`
(the re-stamp is automatic). `currentExecIsAdmin()` is declared in System_AuthIdentity.h, already included
at :35; the return type matches the sibling branch. **~4 lines, one file.**

**Callers checked.** `cmd_logout` has no direct C++ caller. Grep over `components/`, `main/` and `data/`
for `"logout <transport>"` string forms returns **nothing** — no autostart entry, canned lens command, or
internal string issues it. Only humans type it. Bare `logout` is intercepted before dispatch on serial
(HardwareOne.cpp:2694) and BLE (Bluetooth.cpp:779), so ordinary self-logout never reaches this function.
The two other clear+re-stamp sites (`revokeUserSessions`, ban path) are admin-triggered and untouched.

Who loses an ability, honestly: (a) an automation row with an empty stored owner running `logout g2` —
none exists in-tree, device-side `automations.json` not inspectable from here; (b) AuthBypass callers
(physical serial with `serialrequireauth` off, BLE with `bleRequireAuth` off) — that is the point.

**Self-attack.** Fails **closed** — any path reaching `cmd_logout` without going through `executeCommand`
sees the ANON TLS slot and is denied (none found). A lens-typed `logout g2` from an admin-owned lens is a
no-op re-pick; from a *demoted* owner it is now refused, closing a hole. **The audit's own proposed fix was
attacked and rejected**: "permit only the bare `logout` (self-scoped)" rests on a false premise —
`cmd_logout` defaults to `SOURCE_SERIAL` regardless of caller, so a guest typing bare `logout` in the web
CLI still kills the physical serial session. Shipping that would bake in a wrong invariant. Refusing the
command leaves `pairedByUser` exactly as it was (the clear happens *inside* `logoutTransport`, now never
reached), so there is no window where the lens is unowned.

**Residual, each its own finding if you want them:** guest `logout display` (kicks the OLED session),
guest `logout bluetooth` (`bleRevokeAllSessions`), bare `logout` defaulting to `SOURCE_SERIAL` from any
transport, and `login <user> <pass> display` seizing the OLED session.

**Confirm it worked (hardware).** As a guest: `logout g2` → `Error: Admin access required...`, and
`bleautoreconnect`/settings show `pairedByUser` unchanged. As an admin: `logout g2` still succeeds and
re-homes. As a guest: bare `logout` still works.

### 13 — `writeText()` returns `true` unconditionally — **HIGH**

**Defect.** `writeText` (System_Utils.cpp:803, verified verbatim) discards `f.print()`'s byte count and
`f.flush()`/`f.close()` are both `void`, so no write error is observable; it always returns `true`.
`writeTextAtomic` is hardened against *rename* failure only and states "every caller already checks the
returned bool" — an assumption `writeText` violates — so a short or zero-byte `users.json` gets renamed
over the good one and 11 credential-store call sites report success.

**Worse than claimed, and it changes the fix:** the stdio stream is **fully buffered at one LittleFS
block** (`vfs_api.cpp` only calls `setvbuf` when `st_blksize == 0`, and `esp_littlefs` always reports a
nonzero block size). For any `users.json` smaller than one block, `fwrite` absorbs the whole payload and
returns the full count even on a full flash — **a patch that checks only `f.print()`'s return would catch
almost nothing on the real credential path.**

Both downstream outcomes are verified and differ: a **zero-byte** `users.json` is skipped by the boot
corruption sweep (which gates on `content.length() > 0`) while `detectFirstTimeSetupState` tests *existence
only* → reports NOT_NEEDED with no accounts → permanent lockout, reflash required. A **truncated non-empty**
file is quarantined by `loadAndIncrementBootSeq` → setup re-arms → **the device is claimable by whoever
reaches serial or the OLED first.**

**Fix.** Check `f.print()`'s count, then — in a separate scope, **after the close** — reopen and compare
`v.size()` to the expected length. ~25 lines, one file. Optional companion: `VFS::remove(tmp)` on the
`writeTextAtomic` failure path so a failed tmp write doesn't strand a file on already-full flash.

**Self-attack — the first draft was a bricking bug, and this is why hardware testing is mandatory here.**
The specialist's initial version read `f.size()` *before* `f.close()`. `File::size()` calls `_getStat()`,
which does a path `stat()` → `vfs_littlefs_stat` → reads **committed** on-disk metadata; and
`sdkconfig:2878` has `CONFIG_LITTLEFS_FLUSH_FILE_EVERY_WRITE` **unset**, so an open `"w"` handle stats as
size 0 regardless of what was written. That version would have returned false on **every single write**,
including every `users.json` write — turning a rare-failure bug into 100% failure. The verify must happen
after close. docs/AUTH_SECURITY_REVIEW.md:759 says "reopening", and reopening is the only form that works.

Also attacked: deliberately **no** `VFS::remove(path)` on the failure path (the `"w"` open has already
truncated it; deleting would turn a truncated marker into an absent one). No new lock — `FsLockGuard` is
reentrant and `VFS::open` already nests inside it today. Empty content is not a false failure (want=0,
0-byte reopen succeeds, 0 == 0). If the verify reopen itself fails after a good write, the function returns
false — which fails **safe**: `writeTextAtomic` drops the tmp and the live file keeps its old good contents.

**Callers checked.** Only **two** direct `writeText` callers: `writeTextAtomic` itself and
WebServer_MigrationTool.cpp:589 (return already ignored). `writeTextAtomic` has 19 call sites; the 11 on
`USERS_JSON_FILE` are System_User.cpp:916/1050/1130/1362/1430/1677/1801/3332,
System_FirstTimeSetup.cpp:275/908, System_ESPNow.cpp:3695. All of them already handle `false` with an error
return; only System_User.cpp:3200/3392 (boot anchors) and G2_Pet.cpp:303 are fire-and-forget. New cost is
one extra open+stat+close per write, which runs on every successful login via `updateUserLastSeen` — a
small LittleFS stat, not on any ISR or BLE callback path.

**Confirm it worked (hardware — required, do not skip).** First the no-regression leg, because the failure
mode of a wrong implementation is total: create a user, delete a user, reboot, confirm both persisted and
that login still works. Then the failure leg: fill LittleFS to near-full, run `useradd` → must return
`Failed to write users.json` instead of success, and `users.json` must still be valid after reboot.

### 14 — Mesh passphrase decrypted before device-key epoch selection — **HIGH**

**Defect.** In `readSettingsJson`, `espnowMeshesReadJson(doc)` runs at System_Settings.cpp:1394 and
`selectDeviceKeyEpoch(doc)` at :1397 — I confirmed the ordering myself. The mesh reader calls `getSecret`
on the passphrase and stretched key, which reaches `getDeviceEncryptionKey()` and **latches** the key cache
to the default `mac+flashUID` derivation, so on an epoch mismatch both secrets open as empty. The mesh
writer then destroys the recoverable blob: `.to<JsonArray>()` clears the merge-read array and each entry is
a **fresh** object, so `putSecret`'s `prevBlob` is always `""`, `prevIsBlob` is always false, and the
preservation branch (`prevIsBlob && gSecretLoadFailures > 0`) is **structurally unreachable for meshes**.

The safety net is defeated three independent ways: the mesh reader never increments
`gSecretLoadFailures` (grep of System_ESPNow.cpp returns nothing); `selectDeviceKeyEpoch` opens with
`gSecretLoadFailures = 0` and runs *after* the mesh read; and `prevBlob` is `""` anyway. The slot is still
written (the skip guard tests plaintext `label`/`enabled`, which survive), so the loss is **committed**:
`passphrase:""` over a still-recoverable AES blob, stretched key dropped entirely.

**Intent runs the opposite way.** The call-site comment at System_Settings.cpp:1396 literally reads
*"Pick the key epoch that actually opens the stored blobs BEFORE any decrypt"* — while sitting three
statements **below** two decrypting readers. And the WiFi writer 20 lines from the mesh writer names this
precise hazard and handles it: *"Capture the merge-read's on-disk password blobs BEFORE `to<JsonArray>()`
clears the array, so the guarded write below can keep a still-recoverable blob."* The mesh writer captures
nothing.

**Fix — Part 1 is the one I would land alone.** Move `selectDeviceKeyEpoch(doc);` from :1397 to
immediately after `registerAllSettingsModules();` (:1389), so it precedes both `blePeersReadJson` and
`espnowMeshesReadJson`. **3 lines, one file.** It depends only on the parsed `doc` (`collectAesBlobs`,
`ESP.getEfuseMac`, `esp_flash_read_unique_chip_id`, `deriveDeviceKeyFromIds`, `aesBlobDecryptsWith`,
`logSystemEvent` — none need the registry or either reader), and `readSettingsJson` has one live caller
(HardwareOne.cpp:1401), so blast radius is the boot path only. Second-order bonus: it also moves the
`gSecretLoadFailures = 0` reset earlier, which is strictly more correct.

**Parts 2 and 3 — hold.** Part 2 (capture merge-read blobs before the clear, match by label, use
`putSecretPreserving`) and Part 3 (make the mesh reader report failures; needs three new declarations in
System_Settings.h) close the structural hole. Two reasons to hold:

- **Stack.** Part 2 as drafted adds three `String[4]` arrays = **192 bytes**. G2_Glasses.cpp:13261 budgets
  `espnowMeshesWriteJson` at exactly 192 B on a measured worst-case chain totalling 5920 B on the
  `g2_tap_disp` worker (reached inline via `bleStampPairedByIfBlank` → `writeSettingsJson`), and that
  budget is explicitly "a floor, not a total". Part 2 roughly doubles the frame. **Given this project's
  stack history, do not land it without re-walking that budget** — two arrays plus an index match, or
  staging in the PSRAM-backed JsonDocument, both avoid the growth.
- **Ordering dependency.** Part 3 is **inert without Part 1** — `selectDeviceKeyEpoch` resets the counter
  to 0, so any increment the mesh reader makes today is wiped by the next statement. Landing Part 3 alone
  is a no-op that looks like a fix.

**Self-attack.** The dangerous version of Part 2 would skip `.to<JsonArray>()` and mutate in place —
that reintroduces the "empty-list resurrect" class documented at System_Settings.cpp:962, where a deleted
mesh survives the save. The fix keeps the clear and copies blobs out first. One real behavioural edge:
`gSecretLoadFailures` is **global**, so a WiFi decrypt failure elsewhere in the same boot would refuse a
*deliberate* mesh-passphrase clear. That cross-contamination already exists between WiFi and registered
settings and the author chose it deliberately (":560, the RAM emptiness is damage, not intent"), so Part 2
would make meshes consistent rather than invent a new policy — but worst case a cleared passphrase returns
after a damaged boot. Fails toward retention, never toward disclosure. All three web/bond/G2 callers pass
`excludePasswords=true` and every added line is gated on `!excludePasswords`, so no device-key blob escapes.

**UNVERIFIED / honest bound on impact.** The epoch-mismatch precondition was **not observed on this
hardware**, and nobody verified that a real IDF upgrade changes the flash-UID representation. If the epoch
never mismatches, that chain never fires. What is **unconditionally true on every build today** is the
second chain: the preservation branch cannot execute, so *any* cause of an empty-in-RAM passphrase
(corrupt blob, mbedtls failure, PSRAM exhaustion inside `decryptString`) is committed as permanent loss.
That half justifies Part 1 on its own.

**Correcting the prior triage.** docs/AUDIT_TRIAGE.md:438's qualifier ("requires a prior re-key plus a
secret re-save during that boot") belongs to a *different* finding and is wrong twice when carried here:
`espnowMeshesWriteJson` runs unconditionally inside `buildSettingsJsonDoc` on **every** `writeSettingsJson()`
— any of 321 `setSetting(` sites destroys it, no secret need be touched — and the wrong-epoch decrypt
repeats on **every boot** until the first save, not one boot. In the triage's favour: the
SETTINGS_LIFECYCLE_AUDIT premise that boot auto-saves is now stale (HardwareOne.cpp:1455 uses plain
assignment for `crashCount`/`lastResetReason` specifically to avoid it).

**Confirm Part 1 worked (hardware, low risk).** Boot and confirm the mesh still joins and derives its group
key; check the boot log shows the epoch-selection line *before* the mesh read. The mismatch path can't be
readily exercised without forcing a re-key.

### 12 — `handleSettingCommand()` reports success on a failed persist — **HIGH**

**Defect.** All four type branches do `if (!gDeferWrites) writeSettingsJson();` and then unconditionally
`return "[Settings] Configuration updated";` (System_Settings.cpp:2662, :2677, :2687, :2699 — line numbers
confirmed). `writeSettingsJson` has five `return false` exits, four of which log **"settings NOT
persisted"** — so the device logs the failure in the same breath it tells the operator OK.
`cmd_savesettings` (:3082) does the same for the deferred/batch path, which is the **dominant** path for
the web UI (`WebPage_Settings.h:113` wraps every save in `beginwrite ... savesettings`).

~276 registered cmdKeys route here, including auth posture: `cmd_serialrequireauth` /
`cmd_displayrequireauth` are thin wrappers. An operator who turns serial auth ON, is told OK, and reboots
gets it back OFF.

**Not intentional — the codebase's own convention is the opposite, by the same author.**
System_WiFi.cpp:836: *"Returns false when the persist did not happen... callers must not report success on
false, or a RAM-only change silently reverts on reboot."* And System_Notifications.cpp:493/:565/:622 return
the **identical** success string but check their persist first. `handleSettingCommand` is the outlier. It
also breaks the project's own uniform `OK:`/`Error:` contract at its busiest chokepoint.

**Fix.** A file-static `kSettingsPersistFailed` error string (must lead with `"Error"` — `executeCommand`'s
success test is `strncmp(out,"Error",5)`), then in each of the four branches capture
`const bool persisted = gDeferWrites ? true : writeSettingsJson();` (short-circuits exactly like today —
same number of calls) and return conditionally. Plus `cmd_savesettings` returning distinct errors for
settings.json vs debug.json, with `gDeferWrites = false` cleared **first** so a failed flush cannot strand
defer mode on. **~15 lines, one file.** No struct, no signature, no allocation, no lock.

**Callers checked.** All 60+ enumerated: 7 direct plus the `SETTING_EDITOR_CMD` (53 instantiations),
`ESPNOW_SETTING_CMD` and `LLM_SETTING_CMD` macro expansions. Every one is a pure pass-through except three
that sync a live global then `return r` — correct on failure, since the RAM value genuinely did change.
`grep "Configuration updated"` finds the string only at the four sites plus three unrelated notification
commands. **No caller compares against the success string, rolls back, retries, or re-drives.**

**What changes for the user:** existing-but-never-triggered error handling starts firing — the web Settings
page's red first-error banner (WebPage_Settings.h:126, which matches on both `error:` and `failed`, so my
message is caught either way), the G2 failure banner (G2_Page_Settings.cpp:576), and the OLED string-edit
toast (OLED_SettingsEditor.cpp:186 — message is 73 ASCII chars, fits the `char out[128]`, and deliberately
avoids the em-dash used elsewhere so nothing truncates mid-multi-byte). **Pre-existing gap not closed:**
the OLED *numeric* path (:168) is fire-and-forget and still drops errors.

**Self-attack.** Verified no spurious failures — the newly-visible condition set is exactly
`writeSettingsJson`'s five false-returns; there is no path where it returns false but the data persisted.
The deferred case still reports success, so the web batch protocol is unchanged on the happy path. An
attacker who fills LittleFS now gets `Error` instead of `OK` — strictly less useful.

**Scope honesty — this does NOT make the contract airtight.** The rename-fallback at
System_Settings.cpp:1113 calls `serializeJson(doc, directFile)` and discards the byte count, then falls
through to `return true`: a short direct rewrite of the live file still reports success. Separate defect,
also in SETTINGS_LIFECYCLE_AUDIT.md:103.

**Residual, deliberately not patched.** `gDeferWrites` is cleared **only** by `cmd_savesettings`. If a batch
ends before `savesettings` runs, defer mode stays on and every later write is silently RAM-only. The
specialist checked the obvious web path and it does **not** happen (`handleCliBatch` doesn't abort on error,
and `httpenabled off` can't null the server mid-batch because `httpServerStopSafe` defers while
`gWebCmdWaiters > 0`). A `reboot` inside a batch, or a crash mid-batch, still strands it. Auto-clearing is a
design change (timeout? per-session?), not a patch.

**Confirm it worked (hardware).** Happy path first: `beginwrite`, change a setting, `savesettings` →
`Settings saved`, reboot, value persisted. Failure path: fill LittleFS, change a setting → the new
`Error: applied in RAM but settings.json write FAILED - reverts on reboot`, and the web Settings page shows
the red banner instead of green.

---

## 4. Recommended order

| Step | Item | Size | Files | Hardware | Why here |
|---|---|---|---|---|---|
| 1 | **8** — sensorlog admission gate | ~20 lines | 1 | Yes | Only CRITICAL. Any authenticated non-guest user owns the device in minutes. Self-contained, fails closed, no design question blocking it. |
| 2 | **7** — `logout g2` admin gate | ~4 lines | 1 | Yes | Cheapest HIGH in the set. Also removes the escalation leg from design-call 9, which makes that decision easier to think about. |
| 3 | **4 Part 1** — HTML-escape the ESP-NOW sinks | ~15 lines | 1 | Yes (2 devices, or hand-edit stored metadata) | Unauthenticated radio → code execution in an admin session. Part 1 is unambiguous; ship it without waiting on the Part 2 answer. |
| 4 | **14 Part 1** — move `selectDeviceKeyEpoch` up | 3 lines | 1 | Yes (smoke: mesh still joins) | Three lines against permanent, unrecoverable mesh-key loss. Best ratio in the set. Hold Parts 2/3 for the stack re-walk. |
| 5 | **12** — settings persist return values | ~15 lines | 1 | Yes | Mechanical, no caller compares the string, lights up error handling that already exists. Includes `serialrequireauth` silently reverting. |
| 6 | **13** — `writeText` verify-after-close | ~25 lines | 1 | **Yes — mandatory, no-regression leg first** | Highest implementation risk in the set: the obvious implementation (stat before close) fails 100% of writes and bricks persistence. Do it last, when nothing else is in flight to confuse the bisect. |

**Blocked on your answer, no code yet:**

- **4 Part 2** (one line) — was ungated METADATA ingest intended, or did `flags=0` only consider egress?
- **5** — is `espnowpairsecure` == full-filesystem trust, or should the binary FsList path carry the
  per-user token your own comment defers?
- **9** — do you accept SUPER-tier lens taps, and do you accept that BLE has no link-layer security so the
  lens is impersonable by anything in radio range?

**Filed but not in this set** (from the finding-11 verification, which is otherwise dead): the ESP-NOW
receiver echoing raw remote commands pre-auth to a non-admin-readable log feed (already at
AUTH_SECURITY_REVIEW.md:401), and the un-redacted `espnowremote` tail leaking nested passwords to
`command-audit.log` (**not filed anywhere else — file it**).

**Caveat on all of it:** no specialist compiled, flashed, or ran anything. Every "compiles as written" claim
is a header/scope read, not a build.
