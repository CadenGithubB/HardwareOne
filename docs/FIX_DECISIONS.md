# Fix Decisions — 5 candidate defects, adversarially verified

Each candidate was handed to a specialist told to kill it first. Four survived. One died as stated.
Everything below was re-checked against the working tree while writing this document; the line numbers
and code quotes are current as of 2026-08-03.

---

## 1. Verdict table

| # | Candidate | Verdict | One-line reason |
|---|-----------|---------|-----------------|
| 1 | Pre-auth 1-byte stack overflow in `decodeBasicAuth` — `WebServer_Server.cpp:1396-1400` | **GO** | Real, compiled, and worse than reported: the overflowing byte lands exactly on the frame's stack canary, so one unauthenticated HTTP request reboots the device on ~255 of 256 boots. |
| 2 | Full session id printed to a web-readable log sink — `WebServer_Server.cpp:371, 372, 418, 423, 426` (+ `:5071`, found during this pass) | **GO** | The 32-hex bearer token is written to a sink any non-guest can `GET /api/cli/logs`, and `isAuthed` does no IP/UA binding — the token alone authenticates. Fix location is *not* where the candidate said. |
| 3 | Permission check and file I/O canonicalize differently — `normalizeFsPath()` `System_Filesystem.cpp:1468` | **GO** | `/./system/users/users.json` matches no rule, falls to the `PERM_ALL` catch-all, and littlefs then opens the real credential DB. A plain non-admin web account can rewrite `users.json`. |
| 4 | ESP-NOW fragment reassembly does not bind auth to the message — `System_ESPNow.cpp:189` | **GO** | Slots key on `(src,msgId)` only; dispatch inherits the *completing* fragment's verdict, so plaintext forgeries splice into an AEAD-authenticated message. Fix must differ from the proposal (the proposed one is a DoS). |
| 5 | httpd task stacks sized in the wrong unit — `WebServer_Server.cpp:5288, 5382` | **NO-GO** | The unit claim is right (they are BYTES) but the *risk* claim is fabricated — the "measured peak ~18 KB" was never measured, there is no HWM instrumentation on that task, and both values exceed IDF's own defaults. **Change the comments, never the numbers.** |

No candidate landed in RISKY. Three residual judgement calls are flagged inline under §2 (items 2, 3, 4) —
read those before applying, they are the places where a specialist deviated from the candidate as written.

---

## 2. GO items

### GO-1 — Pre-auth one-byte stack overflow in `decodeBasicAuth`

**The defect.** `mbedtls_base64_decode` is handed `sizeof(out_buf)` = 256, and its bound test is
`dlen < *olen` (strict, `esp-idf/components/mbedtls/mbedtls/library/base64.c:220`) — so it *accepts*
`*olen == 256`, writes 256 bytes, and returns 0. The caller then writes the NUL terminator at
`out_buf[256]`, one past the array, on a path that runs before any credential check.

**Why it is certain, not inferred.** Disassembly of the *already-built*
`build/esp-idf/hardwareone/CMakeFiles/__idf_hardwareone.dir/WebServer_Server.cpp.obj` shows
`out_buf` at frame offset 92 with the `-fstack-protector` canary at offset 348. 92 + 256 = 348 exactly.
The overflow does not land on padding — it lands on the canary's low byte. `__stack_chk_guard` is
`esp_random()` per boot, so the low byte is nonzero with p = 255/256, and `__stack_chk_fail()` calls
`esp_system_abort("Stack smashing protect failure!")`. Trigger: `Authorization: Basic <344 base64 chars>`
(~350-byte header, under `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024`). Ordering confirmed at `:604-612` — the
overflow runs *before* `isLoginLocked` and *before* `isValidUser`.

**Fix** — replace `WebServer_Server.cpp:1394-1400`:

```cpp
  // Decode base64. mbedtls_base64_decode's bound test is `dlen < *olen`
  // (mbedtls/library/base64.c:220), so it ACCEPTS *olen == dlen and reserves no
  // room for a terminator. Passing sizeof(out_buf) let a 344-char base64 header
  // decode to exactly 256 bytes and pushed the NUL onto out_buf[256] — which is
  // where -fstack-protector puts this frame's canary. Hand mbedtls one less than
  // the array size so the terminator always has a home.
  size_t out_len = 0;
  unsigned char out_buf[257];
  int ret = mbedtls_base64_decode(out_buf, sizeof(out_buf) - 1, &out_len,
                                  (const unsigned char*)b64.c_str(), b64.length());
  if (ret != 0 || out_len == 0 || out_len >= sizeof(out_buf)) return false;
  out_buf[out_len] = '\0';
```

**Callers checked.** Exactly one call site: `WebServer_Server.cpp:606`, inside `isAuthed()`. Confirmed two
ways — grep across the tree (`:558` is a forward decl, `:1412` and `HardwareOne.cpp:1774` are prose
comments, `HTTPS_Server_Generic`'s `decodeBasicAuthToken` is an unrelated `std::string` member of a
different component), and by scanning all 2338 built `.obj` files for an undefined reference to
`_Z15decodeBasicAuthP9httpd_reqR6StringS2_Rb` — zero hits. Not declared in `WebServer_Server.h`.

**Why nothing breaks.** `out_len < 256` (every real credential): byte-identical. `out_len == 256`: previously
aborted the device, now parses. `> 256` bytes needed: `dlen` is still 256, mbedtls still returns
`BUFFER_TOO_SMALL`, still `return false` — **the accepted credential ceiling stays exactly 256 bytes**.
Frame grows ~16 bytes on a 7,680-byte httpd stack.

**How to confirm it worked.** Log in over the web UI normally (proves the ordinary path is untouched), then
send `Authorization: Basic` with a 344-char base64 blob ending `==`. Before the fix the device panics with
"Stack smashing protect failure!" and reboots; after, it returns a normal 401 and stays up. No hardware-
specific behavior is involved — build-verify is sufficient for correctness, the header probe is the proof.

---

### GO-2 — Full session id leaked to a web-readable sink

**The defect.** `setSession()` prints the complete 32-hex session id to `BROADCAST_PRINTF` / `DEBUG_AUTHF`,
which fan out to serial, the web CLI mirror, and the flash log file. Any authenticated non-guest account can
read that mirror via `GET /api/cli/logs`, and `findSessionIndexBySID` (`:260-266`) plus `isAuthed`
(`:588-726`) validate a cookie on the sid alone — no IP binding, no User-Agent binding, `gSessions[idx].ip`
is never compared against the requester. A "user"-role account polling the endpoint the CLI page already
polls harvests the admin's session on the admin's next login. TTL is 24 h.

**Two sub-claims in the candidate are FALSE — do not act on them.**
1. There is no "per-user output-flag gate." `gOutputFlags` is a single global `volatile uint32_t`
   (`System_Settings.cpp:95`).
2. "The web mirror bypasses the gate" is true as description but is **documented-deliberate** at
   `System_Debug.h:16-24` ("Two sinks are deliberately routing-gated only… Persisted web/display/g2 lane
   settings were removed pre-1.0 once an audit showed delivery had never honored them"). Gating
   `System_Debug.cpp:262` would revert that decision **and fix nothing**, because `MSG_ROUTE_WEB` is raised
   in `startHttpServer` (`:5571`) and cleared in `httpServerStopFinish` (`:137`) — i.e. it is set for
   exactly the HTTP server's lifetime.

The correct fix is in `WebServer_Server.cpp`, applying the `%.8s...` convention the author already uses
40 lines away at `:467-471` and in `redactOutputForLog` (`System_Utils.cpp:1245-1269`).

**Fix** — six format-string edits, values and argument lists unchanged:

```cpp
// :371
          DEBUG_AUTHF("Reusing existing session idx=%d user=%s sid=%.8s... | refreshed", i, u.c_str(), gSessions[i].sid.c_str());
// :372
          BROADCAST_PRINTF("[auth] reusedSession user=%s, sid=%.8s..., exp(ms)=%lu", u.c_str(), gSessions[i].sid.c_str(), gSessions[i].expiresAt);
// :418
  DEBUG_AUTHF("New session created idx=%d user=%s sid=%.8s... | needsStatusUpdate=1", idx, u.c_str(), s.sid.c_str());
// :423
  DEBUG_AUTHF("Setting session cookie for sid=%.8s...", s.sid.c_str());
// :426
  BROADCAST_PRINTF("[auth] setSession user=%s, sid=%.8s..., exp(ms)=%lu", u.c_str(), s.sid.c_str(), s.expiresAt);
// :5071  <-- SIXTH SITE, not in the original candidate; found and verified during this pass
      DEBUG_SSEF("session[%d] sid=%.8s... user=%s needsStatusUpdate=%d lastSeqSent=%lu",
                 i, gSessions[i].sid.c_str(), gSessions[i].user.c_str(),
                 gSessions[i].needsStatusUpdate ? 1 : 0,
                 (unsigned long)gSessions[i].lastSensorSeqSent);
```

The `:5071` site is a session-table diagnostic dump that prints every live sid in full. `DEBUG_SSE` is ON at
boot default (`System_Debug.cpp:46-56`, `kBootDefaultDebugFlags` includes it), as is `DEBUG_AUTH` — so all
six sites are live on a shipping device, not just the two ungated `BROADCAST_PRINTF`s.

Already-correct sites, left alone: `:511` (passes `sidShort`, the truncated form) and `:3284` (passes the
pre-truncated `sidBuf`).

**Callers checked.** `setSession` has three call sites: `:465` (`authSuccessUnified`, uses the return value —
unaffected), `:3652` (`handleLogin`, uses the Set-Cookie side effect — unaffected), `:3706`
(`handleLoginSetSession`, dead: guarded by `if (gSessUser.length() == 0) return;` at `:3696` and `gSessUser`
is never assigned non-empty anywhere in the tree). Nothing parses these log lines — grepping `reusedSession`
and `setSession user` across `components/`, `main/`, `docs/` and tests returns only the emission sites and
audit docs; `WebPage_CLI.h:163,178` renders `/api/cli/logs` as opaque text.

**Why nothing breaks.** `%.8s` is a maximum field width on a `const char*`, safe for shorter strings, no
length guard needed, same construct already compiling at `:469`. Argument types/count/order untouched, so
`-Wformat` is satisfied. The only change is operator-visible text: `sid=a1b2c3d4...` instead of 32 chars.

**Judgement call for you.** An 8-char prefix is still a partial token. With `MAX_SESSIONS = 2` it is
effectively a session *identifier*, and the remaining 24 hex chars stay secret — but if you want zero
disclosure, drop the sid entirely from the two `BROADCAST_PRINTF` lines and keep it only in the
`DEBUG_AUTHF` ones. The patch above is the conservative option that preserves debuggability.

**How to confirm it worked.** Log in, then `GET /api/cli/logs` (or open the web CLI page) and search the
output for a 32-hex run. Before: present. After: only `xxxxxxxx...`. Then confirm login/logout/session-reuse
still work normally, since the fix must not have touched the cookie.

**Note.** Out of scope but same defect class, still open: `docs/AUTH_SECURITY_REVIEW.md:401-407` documents
unredacted credentials on these same sinks from `v4_handle_cmd` (`System_ESPNow.cpp:5781/:5838` not passing
through `redactCmdForAudit`).

---

### GO-3 — Permission check and file I/O disagree on the same path

**The defect.** `normalizeFsPath` (`System_Filesystem.cpp:1468-1504`) rejects `..`, rejects `"` and control
chars, collapses `//`, strips a trailing `/` — and does **nothing** about `.` segments or a missing leading
slash. `lookupRule` (`:1406-1417`) is a bare `startsWith` over rows that all begin with `/`, terminating in
`{nullptr, PERM_ALL, PERM_ALL, PERM_ALL}` (`:1401-1402`). So `/./system/users/users.json` matches no rule,
gets `PERM_ALL` for `FsRole::USER`, and `VFS::normalize` → littlefs then skip the `.`
(`lfs.c:1506-1510`, `// skip '.'`) and open the real credential database.

**Reachability, verified per surface.** `POST /api/files/write` with body
`name=/./system/users/users.json&content=<forged>` needs only a plain authenticated non-guest account:
`tgRequireAuth` (`System_User.cpp:161-173`) tests authentication with no role check,
`webGuestAccessAllowed` (`WebServer_Utils.cpp:409-411`) only filters guests, and the 403 shortcut at `:1890`
runs the *same broken* `lookupRule` so it also returns "not admin-only." Then `:1903` calls
`VFS::openGuarded(name, "w", ctx, /*create=*/true)`. The parent-directory walk survives too — `VFSImpl::mkdir`
on `/.` returns true because littlefs resolves it to root. That is privilege escalation to superadmin in one
request. The same primitive over `/api/files/read`, `/api/files/view`, `/api/files/upload` reads or
overwrites anything. The catch-all also grants `PERM_RENAME` and `canRename` passes `sensitive=false`
(`:1667`), so `/./system/certs/x.key` → `/x.txt` → readable, with flash encryption off on production units.
CLI is admin-gated (`requireQuotedPath` at `:1518` already forces a leading slash), so that surface is
admin→superadmin only.

**Fix** — replace `System_Filesystem.cpp:1468-1504` entirely:

```cpp
bool normalizeFsPath(const String& in, String& out) {
  if (in.length() == 0) return false;

  // Reject ".." anywhere — even resolved-to-here ("/foo/bar/.." should
  // collapse to "/foo" but Arduino LittleFS does not collapse it, so the
  // permission check would see "/foo/bar/.." literally and either bypass
  // the rule (no startsWith match) or behave unpredictably). Easier and
  // safer to reject outright.
  if (in.indexOf("..") >= 0) return false;

  // Paths cross the command line as quoted tokens and the tokenizer has no
  // escape, so a literal double-quote is ambiguous — reject it (and control
  // chars) at this single chokepoint rather than in every producer.
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || (unsigned char)c < 0x20) return false;
  }

  // Emit EXACTLY the string the I/O layer will act on. The rule table is
  // consulted on this string, and lookupRule() is a bare startsWith over rows
  // that all begin with '/', with a trailing catch-all granting PERM_ALL. Any
  // spelling the table fails to recognise therefore lands on the catch-all
  // while VFS::normalize + littlefs still resolve it to the real file — the
  // check and the open disagree. Three spellings did that before this rebuild:
  //   - no leading '/'      (VFS::normalize prepends one)
  //   - a "." segment       (littlefs skips it: lfs.c lfs_dir_find, "skip '.'")
  //   - leading/trailing ' ' (VFS::normalize trims it)
  // Rebuilding from segments makes all three impossible by construction.
  // NOTE: only a segment that is exactly "." is dropped — dotFILES such as
  // /logging_captures/sensors/.anchors.csv must survive untouched.
  String trimmed = in;
  trimmed.trim();
  if (trimmed.length() == 0) return false;

  out = "";
  out.reserve(trimmed.length() + 1);
  const size_t n = trimmed.length();
  size_t i = 0;
  while (i < n) {
    while (i < n && trimmed[i] == '/') i++;           // skip a run of slashes
    const size_t start = i;
    while (i < n && trimmed[i] != '/') i++;           // segment is [start, i)
    const size_t len = i - start;
    if (len == 0) continue;                           // trailing slash(es)
    if (len == 1 && trimmed[start] == '.') continue;  // drop no-op "." segment
    out += '/';
    for (size_t k = start; k < i; k++) out += trimmed[k];
  }
  if (out.length() == 0) out = "/";  // "/", "///", "/.", "/./" all mean root
  return true;
}
```

Output equivalence on canonical inputs (unchanged): `"/"` → `"/"`, `"/sd/"` → `"/sd"`, `"/a//b/"` → `"/a/b"`,
`"/system/x.json"` → unchanged, `"/lc/sensors/.anchors.csv"` → unchanged. Changed, all intended:
`"system/users/users.json"` → `"/system/users/users.json"`, `"/./system/users/users.json"` → same,
`" /system/settings.json"` → `"/system/settings.json"`, whitespace-only → now `false`.

**Callers checked.** One direct caller: `System_VFS.cpp:797` inside `guardedNormalize`. Seven callers of
that: `:814 openGuarded`, `:847 existsGuarded`, `:859 removeGuarded`, `:869/:870 renameGuarded`,
`:886 mkdirGuarded`, `:896 rmdirGuarded` — all pass distinct in/out strings, no aliasing. Downstream:
522 `*Guarded` call sites. None regress, for three independent reasons: (a) zero string literals in the tree
are relative — every one starts with `/`; (b) most run as `FsRole::SYSTEM` or `SUPER`, for which the matched
rule is irrelevant (`permsForRole` `:1592-1606`: every row's `systemPerms` is `PERM_ALL`, and `SUPER`
short-circuits); (c) the non-system sites all compose absolute paths, and the CLI producers pass through
`requireQuotedPath` which already forces a leading slash.

**Three things to consciously accept.**
1. **The `trim()` is the specialist's addition, not the candidate's.** It closes the leading-space variant
   of the identical bug (`" /system/users/users.json"` — 0x20 is not `< 0x20`, so it survives the control-char
   filter). It aligns the check with I/O rather than changing reachability, since `VFS::normalize`
   (`System_VFS.cpp:329`) already trims before every I/O op. Drop the two trim lines for a minimal diff.
2. Whitespace-only input now returns `false` (was `true` with `out = "   "`). Every caller treats false as
   deny + a `[PERM] DENY … reason=path-rejected` log line. No caller passes one.
3. **This does not canonicalize `isAdminOnlyPath` (`:1759`) or `getPermissions` (`:1717`)** — they call
   `lookupRule` on the raw string. After the fix a dotted path can still produce an over-optimistic UI button
   state or a missing friendly 403, but the authoritative `openGuarded`/`existsGuarded`/etc. denies. The
   exploit is closed; the cosmetic inconsistency is not.

**How to confirm it worked.** As a logged-in **non-admin** user, `POST /api/files/read` with
`path=/./system/users/users.json`. Before: returns the credential DB. After: denied, with
`[PERM] DENY … reason=` in the log. Then smoke the ordinary paths that flow through the normalizer: web file
browser listing and view, `files "/"`, `fileview "/system/settings.json"`, a capture write (exercises
`/logging_captures`, `/sd` and the `.anchors.csv` dotfile), and an ESP-NOW file receive into
`/espnow/received`.

**Worth scheduling separately, do NOT bundle:** the trailing catch-all granting `PERM_ALL` to `FsRole::USER`
is what turns every future "no rule matched" mistake into full access. `docs2/OVERLAPS.md:748` proposes
flipping it to deny-by-default with an explicit `/` user-data rule above it. That has real blast radius —
it would immediately bite `/recordings`, `/photos`, `/settings.tmp`.

---

### GO-4 — ESP-NOW fragment reassembly does not bind authentication to the message

**The defect.** `v4_reasm_find_or_alloc` (`System_ESPNow.cpp:189-196`) matches a slot on
`(src, msgId)` only — `type` is written at `:202` and never read anywhere in the tree. The auth verdicts
`wasAuthenticated` / `wasSessionEncrypted` are stack locals of `v4_try_handle_incoming` (`:5165`, `:5169`),
re-initialised per frame; the struct at `:151-162` has no auth field. Fragments are stored as raw bytes
(`:5372-5374`) and the flags handed to `v4_dispatch_table_try` at `:5596-5597` are the *completing*
fragment's. So an attacker in radio range, with a spoofed source MAC (peers are added with
`peerInfo.encrypt = false`, `:926`) and the msgId read in cleartext off the wire header
(`System_ESPNow_Wire.h:209-229` — only the payload is sealed), injects plaintext fragments 1..N-2; the
genuine final SESSION_FRAME then sets both flags true and dispatch runs over a mostly-attacker buffer.

**Every gate before the reassembler is keyless.** CRC16-CCITT is unkeyed and is skipped entirely for
SESSION_FRAMEs (`:5141-5142`). Mesh fingerprint 0 is explicitly allowed (`:5154`). `isPaired` is not
resolved until `:5543`, and `REQ_PAIRED`/`REQ_AUTHENTICATED`/`REQ_SESSION_ENC` live in
`v4_dispatch_table_try` at `:5062-5110` — all *after* reassembly. The duplicate check (`:5354-5358`) is
first-writer-wins, and the genuine fragment is dropped *after* its ACK was already sent (`:5274-5280`), so
the honest sender sees a clean transfer. Sharpest target: `v4_handle_cmd` (`:5714`), whose only crypto gate
is `if (!wasSessionEncrypted)` at `:5740` — a gate whose own comment names token-replay RCE as the threat.

**Do NOT apply the fix as proposed.** "Drop the whole slot on any mismatch" hands an attacker a one-frame
wipe of any in-flight transfer. Use the downgrade-rejecting form below, which never lets a less-trusted
fragment evict a more-trusted slot, and therefore makes honest transfers *more* robust than today.

```cpp
// --- Edit 1: System_ESPNow.cpp:151-162, add two fields to V4ReasmEntry ---
struct V4ReasmEntry {
  bool     active;
  uint8_t  src[6];
  uint32_t msgId;
  uint8_t  type;
  // Cryptographic identity of the message occupying this slot, captured from the
  // fragment that opened it. Every later fragment must present the SAME verdict or
  // it does not belong to this message. Without these, dispatch inherits the verdict
  // of whichever fragment happened to complete the slot (see the flags handed to
  // v4_dispatch_table_try), so a forged plaintext fragment rides out on an
  // AEAD-authenticated message.
  bool     authenticated;     // BROADCAST_AUTH HMAC verified OR SESSION_FRAME unwrapped
  bool     sessionEncrypted;  // AEAD-decrypted SESSION_FRAME (confidential)
  uint8_t  fragCount;
  uint8_t  received;
  bool     have[V4_FRAG_MAX];
  uint8_t  buffer[V4_FRAG_MAX * V4_MAX_FRAGMENT_PAYLOAD];
  uint16_t bufferSize;
  uint32_t lastUpdateMs;
};

// --- Edit 2: System_ESPNow.cpp:166-173, clear the new fields on reset ---
static void v4_reasm_reset(V4ReasmEntry& e) {
  e.active   = false;
  e.msgId    = 0;
  e.received = 0;
  e.fragCount = 0;
  e.type     = 0;
  e.authenticated    = false;
  e.sessionEncrypted = false;
  memset(e.src,  0, 6);
  memset(e.have, 0, sizeof(e.have));
}

// --- Edit 3: System_ESPNow.cpp:189-211, replace v4_reasm_find_or_alloc entirely ---
static V4ReasmEntry* v4_reasm_find_or_alloc(const uint8_t* src, uint32_t msgId, uint8_t type,
                                            uint8_t fragCount, bool authenticated,
                                            bool sessionEncrypted) {
  if (!gV4Reasm) return nullptr;
  for (int i = 0; i < V4_REASM_MAX; i++) {
    if (gV4Reasm[i].active && gV4Reasm[i].msgId == msgId &&
        memcmp(gV4Reasm[i].src, src, 6) == 0) {
      if (gV4Reasm[i].type == type &&
          gV4Reasm[i].authenticated == authenticated &&
          gV4Reasm[i].sessionEncrypted == sessionEncrypted) {
        return &gV4Reasm[i];  // same message — accept the fragment
      }
      // Never let a less-trusted fragment touch a more-trusted message. This is
      // the splice defence: a plaintext frame cannot join an AEAD-protected
      // reassembly. Drop the FRAGMENT, not the slot, so a forged frame cannot
      // deny the genuine transfer — the real fragment for this index still fits.
      if ((gV4Reasm[i].sessionEncrypted && !sessionEncrypted) ||
          (gV4Reasm[i].authenticated   && !authenticated)) {
        WARN_ESPNOWF("[V4_FRAG_RX] rejected fragment %u for msgId=%lu: auth downgrade "
                     "(slot enc=%d auth=%d, frame enc=%d auth=%d)",
                     fragCount, (unsigned long)msgId,
                     (int)gV4Reasm[i].sessionEncrypted, (int)gV4Reasm[i].authenticated,
                     (int)sessionEncrypted, (int)authenticated);
        return nullptr;
      }
      // Equal-or-higher trust with a different opcode, or a trust upgrade: this is
      // a different message reusing (src,msgId) — e.g. a stale STREAM slot followed
      // by the CMD_RESP that shares cmdMsgId, or a genuine encrypted fragment 0
      // arriving after someone pre-seeded the slot in plaintext. Take the slot over
      // instead of merging two messages into one buffer (today's behaviour).
      v4_reasm_reset(gV4Reasm[i]);
      break;
    }
  }
  for (int i = 0; i < V4_REASM_MAX; i++) {
    if (!gV4Reasm[i].active) {
      v4_reasm_reset(gV4Reasm[i]);
      memcpy(gV4Reasm[i].src, src, 6);
      gV4Reasm[i].msgId            = msgId;
      gV4Reasm[i].type             = type;
      gV4Reasm[i].authenticated    = authenticated;
      gV4Reasm[i].sessionEncrypted = sessionEncrypted;
      gV4Reasm[i].fragCount   = (fragCount <= V4_FRAG_MAX) ? fragCount : V4_FRAG_MAX;
      gV4Reasm[i].bufferSize  = (uint16_t)(gV4Reasm[i].fragCount * V4_MAX_FRAGMENT_PAYLOAD);
      gV4Reasm[i].lastUpdateMs = millis();
      gV4Reasm[i].active      = true;
      return &gV4Reasm[i];
    }
  }
  return nullptr;
}

// --- Edit 4: System_ESPNow.cpp:5332-5337, pass the verdict and fix the now-inaccurate log ---
    V4ReasmEntry* e = v4_reasm_find_or_alloc(recv_info->src_addr, h->msgId, h->type, h->fragCount,
                                             wasAuthenticated, wasSessionEncrypted);
    if (!e) {
      WARN_ESPNOWF("[V4_FRAG_RX] fragment not accepted for msgId=%lu — all %u slots in use, "
                   "or it does not match the opcode/auth state of the message already reassembling",
                   (unsigned long)h->msgId, V4_REASM_MAX);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ==============================");
      return true;
    }
```

Both flags are in scope at `:5332` (declared `:5165`/`:5169`, same function). `WARN_ESPNOWF` is already used
in this block. No new includes, no new globals.

**Callers checked.** All state is file-local `static` in `System_ESPNow.cpp`; nothing crosses a TU.
`v4_reasm_find_or_alloc`: exactly one caller (`:5332`) — the signature change is a compile error if missed,
not a silent mis-bind. `v4_reasm_reset`: six sites (`:184`, `:199`, `:5047`, `:5367`, `:5498`, `:5622`),
all behaviour-neutral under the added field-clears. Raw `gV4Reasm` scans (`:5045`, `:5496`, `:5618`) key on
the same `(active, msgId, src)` and only call reset. `sizeof(V4ReasmEntry)` at `:9690` grows 2 bytes × 2
entries = 4 bytes against an ~8.4 KB **PSRAM** block, zero-inited at `:9693`. No DRAM impact.

**Why no honest sender breaks.** `v4_send_encrypted_chunked` (`:2090`) is the only multi-frame sender left
(`v4_send_chunked` was deleted, per the comment at `:273-275`); within one call it holds `type` constant
(`:2169`) and seals every fragment as a SESSION_FRAME (`:2180`), so all fragments present identical
`(type, true, true)`. Legacy whole-plaintext peers present identical `(type, false, false)` and still
reassemble. `TEXT` and `ACK` are excluded from reassembly at `:5316`.

**DoS analysis of the fix itself** (this is why it differs from the proposal): downgrade → rejects the
*fragment*, slot survives, and the genuine fragment for that index still lands because `have[]` was never
set. Upgrade → takes the slot over, and only a real session peer can trigger it. Equal-trust different type
→ takes the slot over; an attacker could evict another plaintext slot, but that is not a new capability —
the 2-entry table with a 5 s timeout and no pre-allocation gate is already trivially exhaustible (filed
separately at `docs/AUTH_SECURITY_REVIEW.md:315-320`).

**Two adjacent defects in the same 100 lines — fix in the same pass.** `:5348` validates
`h->fragIndex >= h->fragCount` against the **wire** fragCount, not the clamped `e->fragCount`, so
`e->have[254]` can be read on a 21-element array and aliases into `buffer[]`. And `:5407-5417` computes
`reassembledSize` from the *completing* frame's `payloadLen`, so out-of-order completion mis-sizes the
delivered buffer. Adding two bools shifts field offsets, which changes *which* bytes the first one aliases —
so fix them together or not at all.

**How to confirm it worked.** This one **needs hardware validation on a two-device mesh** — it is the only
item here that changes radio-path behaviour. Positive control: a bonded `files "/"` listing exercises the
13-fragment `FS_LIST_REPLY` path end to end and must still complete. Negative control: inject a plaintext
fragment with a spoofed src MAC and an observed msgId, and confirm the new
`[V4_FRAG_RX] rejected fragment … auth downgrade` warning fires while the genuine transfer still completes.

**Worth considering instead** (stronger, but a compatibility decision): act on the author's own note at
`:274-275` and simply refuse `fragCount > 1` frames that are not SESSION_FRAMEs. Every current sender is
encrypt-or-fail, so plaintext multi-frame RX buys nothing but attack surface. That drops legacy-peer
tolerance — confirm no un-reflashed peer is in the field first.

---

## 3. RISKY items

None. The three judgement calls that would otherwise sit here are embedded in the GO items above, where the
decision is narrow enough not to block the fix: GO-2's 8-char-prefix-vs-omit-entirely choice, GO-3's added
`trim()` and its uncanonicalized `isAdminOnlyPath`/`getPermissions` siblings, and GO-4's
legacy-plaintext-multi-frame compatibility question.

---

## 4. NO-GO items

### NO-GO-5 — httpd task stacks "sized in the wrong unit"

**Half of this is right; the half that would make you act is fabricated.**

**Real:** the unit is BYTES, and both comments are wrong by 4×. Chain, all read on this machine —
`esp_http_server/src/httpd_main.c:529-534` passes `config.stack_size` unscaled to
`httpd_os_thread_create`; `esp_http_server/src/port/esp32/osal.h:25-30` calls
`xTaskCreatePinnedToCoreWithCaps` with it; `freertos/esp_additions/include/freertos/idf_additions.h:271-272`
documents that parameter as "the number of bytes" and `idf_additions.c:50` allocates
`usStackDepth * sizeof(StackType_t)`; `portmacro.h:88,91` makes `StackType_t` a `uint8_t` on Xtensa
(`CONFIG_IDF_TARGET_ARCH_XTENSA=y`, sdkconfig:391). So the real stacks are 7,680 B and 11,059 B.

**Not real — the entire reason to act:**
- **"Measured peak ~18 KB" was never measured.** There is no `uxTaskGetStackHighWaterMark` on the httpd task
  anywhere in `components/hardwareone/` (20 call sites exist — G2, ESP-NOW, ESP-SR, I2C, main — none for
  httpd). The number is prose.
- **The stacks are adequate.** Measured from the shipped ELF (`entry a1, N` prologues): esp_http_server's own
  frames are 64–176 B; the deepest URI handler frames in the whole build are `handleGPSTracksAPI` 1,648 B,
  `setSession` 1,312 B, `handleFileRead` 1,216 B, `isAuthed` 1,136 B. The deepest nest
  (`handleLogin` → `authSuccessUnified` → `setSession`) is ~2.4 KB before callees. **Nothing is above 2 KB
  in a single frame.**
- **The deep work isn't on this task.** `WebServer_Server.cpp:3306` `submitAndExecuteSync` queues to
  `cmd_exec_task`, which has its own 8,192 B (`HardwareOne.cpp:1605`). The httpd task blocks on a semaphore.
- **IDF's own defaults are lower** — `esp_http_server.h:55` `.stack_size = 4096`,
  `esp_https_server.h:153` `.stack_size = 10240`. Both current values exceed them.
- **An overflow would not be silent.** `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y` (sdkconfig:1977) aborts
  naming the task. 7,680 B has been live since 2026-05-09 across v0.99.0–v0.99.7 with no httpd overflow.
- **In-tree corroboration:** `WebServer_MigrationTool.cpp:919/:944` run the FTS restore server at 8,192 B,
  no words claim, and it works.

**The dangerous action is the "obvious" one.** Multiplying by 4 to honour the comment (44,236 and 30,720)
requests that much *contiguous internal DRAM* (`task_caps` defaults to `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT`).
On this device that allocation very likely fails, and `httpd_start` returning `ESP_ERR_HTTPD_TASK` means
`:5387-5392` logs "web server FAILED to start" and **the entire web UI is gone**. On the HTTPS side the
failure is quieter — it falls back to plain HTTP, so the symptom is "HTTPS silently stopped working."
`docs/ESP32_PITFALL_AUDIT.md:1604` and `:1799` independently reached this same warning. Latent hazard, not
currently triggered: `httpd_config_t.stack_size` is `size_t` but `osal.h:26` takes a `uint16_t`, so any
future value over 65,535 truncates silently.

**So: NO-GO on any sizing change. The comments are still worth rewriting**, as a LOW-priority doc fix —
they are load-bearing misinformation on a live config value, they already caused one silent 30% reduction
(commit `0b3c218` cut 11,059 B → 7,680 B while believing it was going 44 KB → 30 KB; `993d9c08`'s original
comment was *correct*), and the myth has now propagated into a security document:
`docs/AUTH_SECURITY_REVIEW.md:1206` asserts "30 KB plain HTTP / 44 KB HTTPS" — correct that parenthetical to
"7,680 B plain HTTP / 11,059 B HTTPS". The project already fixed this identical error at
`HardwareOne.cpp:1605` and `G2_Glasses.cpp:11950/:12303`; these two sites are the un-swept remainder.

**Not intentional, but not urgent.** No comment anywhere documents "words" as a design choice — the comments
assert it as fact and are simply wrong. If you want a real improvement beyond prose, add
`uxTaskGetStackHighWaterMark()` reporting for the httpd task so the next person has a number instead of a
story.

---

## 5. Ordering

**Severity order** (worst hole first): **3 → 4 → 1 → 2 → 5.**
**Risk-adjusted execution order** (what I'd actually do): **1+2 → 3 → 4 → 5.**

| Order | Item | Files | Lines touched | HW test needed? | Why here |
|-------|------|-------|---------------|-----------------|----------|
| 1 | GO-1 + GO-2 batched | 1 (`WebServer_Server.cpp`) | ~4 code + ~6 comment (GO-1); 6 format strings (GO-2) | No | Same file, one editing session. Neither can alter a non-crashing code path — GO-1 changes only a buffer size and a bound, GO-2 changes only characters inside format strings. Highest certainty-to-effort ratio in the set; do them while deciding on the bigger two. |
| 2 | GO-3 | 1 (`System_Filesystem.cpp`) | ~37 replaced by ~45 in one function | Smoke only | **The worst hole here** — a plain non-admin web account rewriting `users.json` is superadmin in one request. It is second in execution order only because it is a whole-function rewrite that deserves the equivalence table in §2 to be re-read before it lands, not because it is less urgent. If only one thing ships this week, make it this. |
| 3 | GO-4 (+ the two adjacent reassembler defects) | 1 (`System_ESPNow.cpp`) | ~60 across 4 edits, plus ~4 for the two adjacent bugs | **Yes — two-device mesh** | Largest diff, the only one that changes radio-path behaviour, and the only one that cannot be validated from a build. Bundle the `have[]` wire-fragCount bug (`:5348`) and the `reassembledSize` bug (`:5407-5417`) into the same pass — the struct change moves which bytes the first one aliases. |
| 4 | NO-GO-5 comments | 1 code (`WebServer_Server.cpp`) + 1 doc (`AUTH_SECURITY_REVIEW.md:1206`) | ~20 comment lines, **0 code** | No | Zero generated-code change. Last because nothing runtime depends on it — but do it, because the wrong comment already caused one accidental stack reduction and has leaked into a security doc. |

**Total if all four land:** 3 source files, ~120 lines, of which roughly half are comments. Only GO-4 requires
hardware. Per the project's standing rule, no edits, builds, or git operations were performed while
producing this document — everything above is read-only analysis awaiting your approval.

**Two corrections to existing docs, independent of any fix:**
- `docs/AUTH_SECURITY_REVIEW.md:1206` — "30 KB plain HTTP / 44 KB HTTPS" is the 4× myth; real values are
  7,680 B and 11,059 B. This makes GO-1 slightly *more* urgent, not less.
- `docs/PRE_1_0_HARDENING_AUDIT.md:75` — "traversal blocked by `normalizeFsPath` + role rules" is wrong on the
  role-rule half; GO-3 is exactly the counterexample. `docs/AUTH_SECURITY_REVIEW.md:2293` ("`users.json` is
  NOT reachable by a non-admin… verified by tracing `lookupRule` row-by-row") is true only for the canonical
  spelling and is superseded by GO-3.
- The in-code comment at `WebServer_Server.cpp:1391` and `HardwareOne.cpp:1775` both cite
  `AUTH_ASSESSMENT_REPORT.md`, which does not exist anywhere in the tree.
