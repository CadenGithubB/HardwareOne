# Hot-Path Heap Fragmentation Audit

> ## ⚠️ Status / resume here — paused 2026-07-16 (usage limit)
>
> **This document is mid-revision. Do not act on it without reading this box.** Report only; **no source
> file was modified and nothing was committed.**
>
> ### What is solid
> - **The three String mechanics** (below) — verified by direct inspection of `sdkconfig` and
>   `components/arduino/cores/esp32/WString.{h,cpp}`. Trustworthy.
> - **The `readText()` per-byte finding** — hand-verified. `fs::File` does not override `readString`
>   (zero hits in `components/arduino/libraries/FS/src/`), so every whole-file read runs
>   `Stream::readString`'s per-byte `ret += (char)c`. Real.
> - **27 CONFIRMED findings**, each passed two independent adversarial lenses (hotness + allocation arena).
>
> ### What is NOT solid — three known defects
> 1. **Every "Fix:" line in this document is UNVERIFIED.** The audit adversarially verified *findings* but
>    never verified *fixes*. This is not hypothetical: the original #1 fix (`out.reserve()` before
>    `out = f.readString()`) was **proven wrong** by hand — see the correction in short-list item 1 and S1.
>    A `FixVerify` pass covering 11 prescriptions was launched and **cut off before returning any results**.
> 2. **The framing in this document is contested — and the challenger is probably right.** The 18 KB
>    fragmentation-mechanics analysis never reached the synthesis pass (payload truncation) and concludes
>    the opposite of this doc: *"String churn is the stirrer, not the killer… the churn is the weather, the
>    ratchets are the damage."* It argues the real problem is **contiguity** (largest free block ~9 KB
>    against ~29–39 KB free; ~12 per-UI-action G2 workers each demanding a fresh 4–8 KB *contiguous* block),
>    and ranks String/`reserve()` work **5th**. It is recovered in full at
>    **`HOT_PATH_HEAP_AUDIT_MECHANICS.md` — read that first.** The short list below has **not** been
>    re-ranked against it.
> 3. **The counts here are incomplete.** The synthesis payload truncated: the REFUTED bucket (17), part of
>    DISPUTED, and 10 critic findings never reached it. Critic findings are preserved in the mechanics
>    companion doc; the refuted bucket survives only in the run journal (see below).
>
> ### Two unverified NON-allocation bugs tripped over en route — worth checking on their own merits
> - **[System_Automation.cpp:2282](components/hardwareone/System_Automation.cpp#L2282)** — `nextFire()`'s
>   `intervalMs / 1000` integer division has no floor. An interval under 1000 ms reportedly yields
>   `nextAt == firedAt` → permanently due → a full read+parse+serialize+**flash write** every main-loop pass.
>   Flash-wear risk. **Unverified** — confirm such an interval can actually be set from any interface.
> - **[WebServer_Server.cpp:4811](components/hardwareone/WebServer_Server.cpp#L4811)** — the batch endpoint
>   reportedly returns the **unredacted** output to the HTTP client while only the broadcast sink gets the
>   redacted copy. Possible credential disclosure. **Unverified.**
>
> ### 🔒 Security claim — verify before touching the PSRAM knob
> The mechanics analyst claims **`gSessions` (ESP-NOW AEAD session keys) already lives on the PSRAM heap**,
> violating the standing "never put secrets in PSRAM" rule (flash encryption is **off**, so PSRAM is
> plaintext on an externally probeable chip). It further warns that lowering
> `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` — its own #1 structural recommendation — would **silently relocate
> short secret allocations (32 B keys, session tokens, typed passwords) into plaintext PSRAM**, since today
> they are protected only *by accident* (they're small, so the 16384 threshold keeps them internal).
> **Unverified. Confirm before any sdkconfig change.**
>
> ### To resume
> 1. Read `HOT_PATH_HEAP_AUDIT_MECHANICS.md` and decide the contiguity-vs-String framing. Re-rank this list.
> 2. Re-run the fix-verification pass over the 11 prescriptions (`readText` correct fix, `isAdminUser` cache
>    vs `gIdentityGeneration`, `findCommand` strncasecmp, `authorizeCommand` hoist, automations tick,
>    `VFS::open` double-normalize, `buildFilesListing`, `redactCmdForAudit` early-out, the two bugs above,
>    the `gSessions`/ALWAYSINTERNAL security gate).
> 3. Workflow script (11 fixverify prompts already written):
>    `~/.claude/projects/-Users-morgan-esp-hardwareone-idf/50e6450c-fb74-4893-85df-c66355ee6067/workflows/scripts/hot-path-heap-audit-wf_ab2a3338-292.js`
>    Raw per-agent results incl. the refuted bucket: the sibling `subagents/workflows/wf_ab2a3338-292/journal.jsonl`.
>    **Note:** `resumeFromRunId` is same-session only — in a new session the cache is gone and the script
>    re-runs from scratch (~213 agents). Everything worth keeping is already in these two docs.

_Generated 2026-07-16 · first-party firmware only (`components/hardwareone/`, `main/`) · vendored libraries (`components/arduino`, `components/esp32-camera`), generated game blobs, and `build/` out of scope._

## Read this first: the three mechanics

Every finding below is real *because of* these three properties of this specific firmware. Without them, most of what follows would be noise. Verify them yourself before acting on anything here.

### 1. The 16 KB ALWAYSINTERNAL threshold — every String is internal DRAM

`sdkconfig` has `CONFIG_SPIRAM_USE_MALLOC=y` with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`.

**Every `malloc` / `new` / Arduino `String` allocation smaller than 16384 bytes is served from internal DRAM.** Only allocations *larger* than 16 KB can reach PSRAM through plain `malloc`. Internal DRAM is the scarce, tight, fragmentation-prone heap on this device.

Consequence: `PSRAM_JSON_DOC(doc)` correctly places the *document* in PSRAM — and then `String out; serializeJson(doc, out);` throws that away by materializing the whole payload back in the scarce heap. That pattern appears repeatedly below.

`ps_alloc(..., AllocPref::PreferPSRAM, ...)` uses `heap_caps_malloc(MALLOC_CAP_SPIRAM)` directly, bypasses the threshold, and is genuinely PSRAM — but it falls back to internal DRAM when PSRAM is exhausted.

### 2. The 14-character SSO threshold — short strings are free

Arduino `String` (`components/arduino/cores/esp32/WString.h:326`) has small-string optimization. `SSOSIZE = sizeof(_ptr) + 4 - 1 = 15`; usable inline capacity is **14 characters**.

`changeBuffer` takes the free inline branch only when `maxStrLen < sizeof(sso.buff) - 1`, i.e. **strictly less than 14**. So:

- `String("ok")`, `"on"`, `g2status` (8), `ringstatus` (10), short flags — **never allocate. Free. Not findings.**
- Command lines with args, file paths (`/system/sys_logs/events.log` = 27), MAC display strings (`AA:BB:CC:DD:EE:FF` = 17), JSON, HTML, log lines, URLs, session tokens (32) — **all allocate.**

This filter kills roughly half the naive "String is slow" findings. It is applied throughout.

### 3. Exact-fit 16-byte-granular growth — `+=` in a loop is the real driver

`String::changeBuffer` computes:

```c
size_t newSize = (maxStrLen + 16) & (~0xf);
realloc(isSSO() ? nullptr : wbuffer(), newSize);
```

This is **16-byte-granular exact-fit growth, not geometric doubling.** Building a 1 KB string by appending costs **~64 reallocs**, each of which may memcpy and relocate an ever-larger block. A 4 KB file costs ~256. Each `a + b + c` chain also materializes intermediates.

**This is the #1 fragmentation driver in this codebase, and its worst instance is not a loop anyone wrote.** See the systemic section.

### Scope note

Task stacks are BYTES not words despite `*_STACK_WORDS` naming — known, documented, not re-flagged here.

**No backwards compatibility is required.** The owner erases before flashing. Every fix below can be a clean breaking change: change signatures, delete the String overloads, no shims, no migration paths, no version gates.

---

## Method

A **27-lane sweep** across the first-party tree. Every steady-state candidate was then adversarially verified on **two independent lenses**:

- **Hotness lens** — does this actually run repeatedly in steady state? Traced to the task loop, packet callback, or dispatch site. "Looks hot" was rejected.
- **Arena lens** — does this actually allocate from internal DRAM? SSO filter, stack-buffer check, `ps_alloc` check, RVO/move-elision check.

Both lenses had to fail to refute for **CONFIRMED**. One lens refuting yields **DISPUTED** — the disagreement itself is recorded, because it is almost always a frequency-tier miscalibration on a real allocation.

**Results: 27 confirmed (21 unique sites; four sites were independently rediscovered by 2–3 lanes) · 33 disputed.**

The disputed set is where the value is concentrated for a reader deciding what *not* to do: 24 of 33 were refuted on frequency, not on mechanism. The code is real, the allocation is real, and it runs when nobody is looking.

A caveat on provenance: the verification result set arrived truncated at the tail of the disputed array, so a separately-tallied refuted bucket (if one existed) did not reach me. The REFUTED appendix below is therefore drawn from the refuting lens of disputed findings, which is where the substantive dismissals live. I have re-verified the load-bearing dismissals by hand rather than pass them through.

---

## The short list — ranked honestly

Ranked by frequency × bytes × confidence. **Items 1–4 are worth doing. Items 5–8 are worth doing while you are already in the file. Everything below the line is rounding error and is labeled as such.**

### 1. `readText()` reads every file one byte at a time — fix this first

[System_Utils.cpp:804](components/hardwareone/System_Utils.cpp#L804) · `out = f.readString();`

`fs::File` does **not** override `readString`. I grepped the entire `components/arduino/libraries/FS/src/` tree — zero hits. So it inherits `Stream::readString`:

```c
String Stream::readString() {
  String ret;
  int c = timedRead();
  while (c >= 0) {
    ret += (char)c;      // <-- one byte at a time
    c = timedRead();
  }
  return ret;
}
```

Against mechanic #3 that is **one realloc per 16 bytes of file**, each potentially relocating a growing internal-DRAM block. A 2 KB `users.json` costs ~128 reallocs. A 4 KB `automations.json` costs ~256.

This single function is the upstream of findings 2, 3, and 5.

> ### ⚠️ CORRECTION (2026-07-16) — the fix originally published here was WRONG
>
> This section used to say "**one line fixes all of them:** `out.reserve(f.size() + 1);`". **That does not
> work.** Verified by hand-reading `WString.cpp`:
>
> - The per-byte churn happens inside `Stream::readString()`'s **own local** `String ret`. `out` is only
>   move-assigned from it at the very end. Reserving `out` cannot affect the local's growth at all.
> - It is also mildly **counterproductive**: `String::move()` has an `if (capacity() >= rhs.len())` branch
>   that **memmoves** rhs's content into out's preallocated buffer and frees rhs's, instead of cheaply
>   stealing the pointer. So reserving means you pay the ~N/16 reallocs **plus** a full memmove **plus** a
>   free.
>
> The **finding is real** — `readText()` genuinely reads every file one byte at a time. Only the
> prescription was broken. The correct fix must bypass `readString()` entirely, e.g.:
>
> ```c
> // reserve ONCE, then fill via chunked concat — concat's internal reserve() short-circuits
> // because capacity is already sufficient, so this is 1 allocation total instead of ~N/16.
> const size_t n = f.size();
> out = "";
> out.reserve(n + 1);
> char buf[512];
> int r;
> while ((r = f.read((uint8_t*)buf, sizeof(buf))) > 0) out.concat(buf, r);
> ```
>
> **This corrected fix is itself UNVERIFIED** — the fix-verification pass was cut short by usage limits
> before it returned. Confirm `String::concat(const char*, unsigned int)` exists and that `reserve()`
> short-circuits inside it before relying on this. See "Status / resume here" at the top of this document.
>
> Root cause of the error: the audit adversarially verified **findings** but never verified **fixes**. Treat
> every other "Fix:" line in this document as unverified until that pass is re-run.

### 2. `isAdminUser()` re-reads `users.json` off flash on every call

[System_User.cpp:276](components/hardwareone/System_User.cpp#L276) · `if (!readText(USERS_JSON_FILE, json)) return false;`

Three independent drivers, all traced:

- **Per-command** — `executeCommand` → `CommandIdentityScope scope(ctx)` ([System_Utils.cpp:4184](components/hardwareone/System_Utils.cpp#L4184)) → `ExecIdentityGuard` ctor → `slot->isAdmin = isAdminUser(install.user)`. Unconditional, every transport.
- **Per-FS-op** — `resolveRole()` ([System_Filesystem.cpp:1452](components/hardwareone/System_Filesystem.cpp#L1452)) backs every `VFS::*Guarded` call.
- **Per-directory-entry** — `getPermissions()` calls `resolveRole()` once per entry inside the `openNextFile()` listing loop. **An N-entry directory listing = N full file reads.** The in-source comment at `System_Filesystem.cpp:1586` even says *"Hot path — called once per entry by buildFilesListing"* — and then leaves a whole-file read inside the loop.

Cost: `~filesize/16` realloc+relocate per call, plus two substring temporaries per user record, plus a `String(path)` (24 chars) and a `systemAuth` ctx.path (`"system:user.isAdmin"`, 19 chars).

The redundancy is self-evident **inside the lane**: `ExecIdentityGuard` already memoizes the verdict into the task's TLS slot and exposes it via `currentExecIsAdmin()` — and `resolveRole()` ignores that cache and hits flash anyway. `isUserBanned()` ([System_User.cpp:846](components/hardwareone/System_User.cpp#L846)) already does this correctly against the *same file* with a streaming `PSRAM_JSON_DOC` + `deserializeJson(doc, f)`, never materializing a String. **isAdminUser is the outlier, not the norm.**

**Fix:** cache the admin bit against `gIdentityGeneration` — the invalidation protocol this lane already owns and documents at `System_AuthIdentity.h:255`. `bumpIdentityGeneration()` already fires on exactly the events that can change an admin verdict (add/approve/delete/promote/demote) and deliberately does *not* fire on login/logout/password-change. A 4–8 entry `{user, isAdmin, gen}` memo is provably correct and collapses steady-state cost to zero.

### 3. `findCommand()` builds a String from every registry entry, twice per command

[System_Command.cpp:103](components/hardwareone/System_Command.cpp#L103)

```c
for (size_t i = 0; i < commandRegistrySize; i++) {
  const char* entryName = commandRegistry[i]->name;
  size_t entryLen = strlen(entryName);

  String lcEntry = String(entryName);   // <-- heap alloc per entry
  lcEntry.toLowerCase();

  if (lc.startsWith(lcEntry)) {
```

The loop has **no early exit and structurally cannot have one** — longest-prefix semantics (`entryLen > bestLen`) require a full scan.

I measured the registry directly rather than trusting the estimate: **1222 candidate names, 263 of them ≥15 chars.** Those 263 exceed SSO and allocate; the other ~959 are inline and free. So ~263 malloc/free pairs per walk.

And the walk happens **twice per dispatch**, unconditionally:
- [System_Utils.cpp:4200](components/hardwareone/System_Utils.cpp#L4200) `authorizeCommand` → `commandRequiresAdmin` → [System_Utils.cpp:3145](components/hardwareone/System_Utils.cpp#L3145) `findCommand`
- [System_Utils.cpp:4334](components/hardwareone/System_Utils.cpp#L4334) `found = findCommand(command)` — `found` is declared `nullptr` at :4313, so the `if (!found)` guard is always true.

**Honest cost framing — this matters and both verifiers flagged it:** `lcEntry` is destroyed at the end of each iteration *before* the next is constructed, with zero interleaved allocations. The heap hands back the same block 263 times consecutively. Peak resident footprint is **~32 bytes, not 8 KB**, and a same-size alloc/free pair with nothing interleaved is the most benign pattern a heap can see. **This is not a fragmentation driver. It is a CPU driver:** ~526 malloc/free round-trips + ~1886 `strlen` + ~1886 `strncmp` per command dispatch.

Fix it anyway — it is free to fix and it is on the universal chokepoint:

```c
if (cmdLine.length() < entryLen) continue;        // integer reject
if (entryLen <= bestLen) continue;                // longest-match reject
if (strncasecmp(cmdLine.c_str(), entryName, entryLen) == 0) { ... }
```

Zero allocations, and it lets you drop the `String lc = cmdLine; lc.toLowerCase();` copy at :91 too.

### 4. Hoist the redundant `findCommand` walk out of `authorizeCommand`

[System_Utils.cpp:4069](components/hardwareone/System_Utils.cpp#L4069)

```c
static bool authorizeCommand(const AuthContext& ctx, const String& line, char* out, size_t outSize) {
  if (commandRequiresAdmin(line) && !hasAdminPrivilege(ctx)) {
```

`commandRequiresAdmin` is a two-line wrapper that does nothing but `findCommand(cmdLine)` and read `entry->requiresAdmin` — fetching the *same* `CommandEntry` that `executeCommand` looks up again 130 lines later.

`commandRequiresAdmin(line)` is the **left operand of `&&`**, so it runs on every command regardless of admin status.

**Fix:** call `found = findCommand(command)` **once** at the top of `executeCommand`, pass the resolved `const CommandEntry*` into `authorizeCommand`, read `found->requiresAdmin` directly, and reuse the pointer at :4334. This **halves dispatch cost before you even fix #3** — and combined with #3 it takes the path to zero allocations.

### 5. `automations.json` is read 2–3× per scheduler tick

[System_Automation.cpp:3684](components/hardwareone/System_Automation.cpp#L3684) (tick) · [:520](components/hardwareone/System_Automation.cpp#L520) (`rebuildAutoCache`) · [:2414](components/hardwareone/System_Automation.cpp#L2414) (`rescheduleAfterFire`)

Each is a full `readText` — i.e. the byte-at-a-time growth of #1 — of a file the tick **already holds in a local**. `rescheduleAfterFire` then serializes the whole document *back* into a String via `serializeJsonPretty`, which flushes 32-byte chunks through `String::concat` and forces a fresh 16-byte realloc per flush: a second full-document growth cycle per clock fire.

**Cadence, honestly:** the tick is gated at [HardwareOne.cpp:2211](components/hardwareone/HardwareOne.cpp#L2211) by `gAutosDirty || automationsAnyDue(nowT) || (automationEventsPending() && >=250ms) || (>=60000ms)`. The guaranteed floor is the **60 s safety interval**, not per-loop-pass — the cheap in-RAM `automationsAnyDue()` array scan exists precisely to prevent that. It bursts to 4/sec only with a user-configured event-triggered automation on a chatty source. So: 2–3 full-file byte-append cycles per minute guaranteed, up to 12/sec worst case.

**Fix:** pass the tick's `json` into `rebuildAutoCache(const String&)` and `rescheduleAfterFire(...)`. Re-read only when a fire actually persisted a new `nextAt`. Plus #1's `reserve`.

### 6. `buildFilesListing` accumulates the whole JSON body with no `reserve()`

[System_Filesystem.cpp:364](components/hardwareone/System_Filesystem.cpp#L364)

```c
out += "{\"name\":\"";
for (size_t ci = 0; ci < fileName.length(); ci++) {
  char c = fileName.charAt(ci);
  if (c == '"' || c == '\\') out += '\\';
  out += c;                                    // char-at-a-time
}
out += "\",\"type\":\"folder\",\"size\":\"";
```

~95–125 reallocs for a 20-entry directory, each relocating an ever-larger block — the textbook driver of mechanic #3. Then [:516](components/hardwareone/System_Filesystem.cpp#L516) copies the finished `body` wholesale into `out`, so **two full copies of the listing are live in internal DRAM at peak**.

The mount-point branch at :296 already does the right thing (snprintf into a stack buffer). The real-entry branches just don't follow it.

**Fix:** `out.reserve(2048)` after the `out = ""` at :269 kills the realloc chain immediately. Then have `buildFilesListJson` write its envelope prefix into `out` and pass `out` straight down, deleting the `body` temporary and the doubled peak.

**Frequency caveat:** every driver is user-initiated (folder click, settings page load, typed `files json`). An idle device never calls it. Fix it because it is cheap and it is the worst *shape* in the tree, not because it runs at idle.

### 7. `VFS::open` normalizes the path twice — on every FS operation

[System_VFS.cpp:402](components/hardwareone/System_VFS.cpp#L402)

```c
File open(const String& path, const char* mode, bool create) {
  String p = normalize(path);
  ...
  if (getStorageType(p) == SDCARD) {
```

`getStorageType(p)` ([:315](components/hardwareone/System_VFS.cpp#L315)) calls `normalize()` **again** on the already-normalized `p`, and `normalize()` opens with an unconditional `String p = path;` copy. Repeats identically in `exists()`, `mkdir()`, `remove()`, `rename()`, `rmdir()`.

Log paths are 27–34 chars → 32–48 B blocks, all internal DRAM. `appendLineWithCap` calls `VFS::open` **twice** per log line ([:1702](components/hardwareone/System_Filesystem.cpp#L1702) append, [:1718](components/hardwareone/System_Filesystem.cpp#L1718) size-check read), and each open costs 3 allocs (call-site `String(dest)` + 2 normalize copies) → **6 internal-DRAM allocs per logged line.** The guarded path is worse: `openGuarded` normalizes, then `open` normalizes, then `getStorageType` normalizes — 3×.

**Fix, two independent halves:**
- Split out `static StorageType storageTypeOfNormalized(const String& p)` that does the `p == "/sd" || p.startsWith("/sd/")` test with no copy; call it from open/exists/mkdir/remove/rename/rmdir. Keep the public `getStorageType` as a normalize-then-delegate wrapper. Zero behavior change.
- Add a `const char*` overload of `VFS::open`/`openGuarded`. `dest` in `appendLineWithCap` is already a `char[128]` from `resolveOverflowPath`; the String round-trip buys nothing. This is a clean breaking change — no compat needed.

Together: 6 allocs per log line → 0.

### 8. `redactCmdForAudit` makes two full copies before checking whether any rule applies

[System_Utils.cpp:1146](components/hardwareone/System_Utils.cpp#L1146)

```c
String redactCmdForAudit(const String& argsInput) {
  String c = argsInput;
  String cl = c; cl.toLowerCase();

  for (size_t i = 0; i < (sizeof(kRules) / sizeof(kRules[0])); ++i) {
    const RedactRule& r = kRules[i];
    if (!cl.startsWith(r.prefix)) continue;
```

`kRules` is 18 credential-bearing prefixes (`wifiadd `, `login `, `useradd `…). The overwhelmingly common case matches nothing and returns the copy untouched — having paid for two full copies and a `toLowerCase` for nothing. Called 2–3× per command (`logCommandExecution` at [:921](components/hardwareone/System_Utils.cpp#L921), the audit block, and `appendCommandToFeed` on web/serial).

Independently rediscovered by **three separate lanes**, which is why it is here despite modest cost.

**Fix:** `strncasecmp(argsInput.c_str(), r.prefix, strlen(r.prefix))` against the original; only construct `String c` inside the branch that actually rewrites; early-return `argsInput` on no-match. Add a `const char*` overload so :921 stops building a third temporary just to bind the `const String&`.

---

### Below the line — real, confirmed, and not worth prioritizing

These all passed both lenses. They are listed for completeness and should be swept up opportunistically, not scheduled. **Do not let them displace items 1–8.**

| Site | Why it's rounding error |
|---|---|
| [System_ESPNow.cpp:8202](components/hardwareone/System_ESPNow.cpp#L8202) `parseMacAddress` at 100 Hz | 32 B same-size alloc/free, nothing interleaved — the most benign shape a heap can see. Bond-master-only (all three gates default off). Burns allocator CPU, not heap integrity. |
| [HardwareOne.cpp:494](components/hardwareone/HardwareOne.cpp#L494) `appendCommandToFeed` concat | ~4 transient blocks at human typing cadence. One concat, not a loop. |
| [System_LLM.cpp:1340](components/hardwareone/System_LLM.cpp#L1340) `strdup(prompt)` / [:1441](components/hardwareone/System_LLM.cpp#L1441) `prompt_tokens` | Both verifiers: the "~4 KB hole" is the 1024-char *cap*, not typical. A real chat turn is ~20–40 chars → ~92 B. And the 1 KB strdup is strictly nested inside the 4 KB `prompt_tokens` on the same call — it cannot fragment. Human-paced (one turn, seconds apart). |
| [WebServer_Server.h:250](components/hardwareone/WebServer_Server.h#L250) `makeWebAuthCtx` | 32 B per HTTP request. Fix is a wide-blast-radius signature change for one small block. |
| [System_Automation.cpp:3911](components/hardwareone/System_Automation.cpp#L3911) `"IF " + condition + " THEN _"` | One ~32 B block per fire. Sits on the same line of execution as a whole-file parse that dwarfs it. Fixing this alone buys nothing. |
| [System_Automation.cpp:93](components/hardwareone/System_Automation.cpp#L93) `uc.line = cmd` | 16–32 B, freed LIFO in the same scope; the `ExecReq` allocated between them targets PSRAM so it doesn't even interleave on the DRAM heap. |
| [G2_Page_ESPNow.cpp:1238](components/hardwareone/G2_Page_ESPNow.cpp#L1238) `new RedrawSpec` | 4 bytes. Genuinely per-fragment, but gated on the wearer staring at one specific inbox sub-page. |

---

## Confirmed findings by frequency tier

Each finding carries the corrected tier and arena from the two-lens verification, not the originally claimed one.

### per-tick

#### [System_ESPNow.cpp:8202](components/hardwareone/System_ESPNow.cpp#L8202) — `processMeshHeartbeats` bond sync tick · **CONFIRMED**

```c
bool macOk = (gSettings.bondPeerMac.length() > 0 && parseMacAddress(gSettings.bondPeerMac, peerMac));
```

`espnowHeartbeatTaskFn` ([:8642](components/hardwareone/System_ESPNow.cpp#L8642)) is a bare `for(;;) { processMeshHeartbeats(); vTaskDelay(pdMS_TO_TICKS(10)); }` → 100 Hz. Both verifiers independently grepped 7773..8202 for an early return and found only the `!gEspNow->initialized` guard; the 3 s cooldown and the `macOk &&` short-circuit are both *downstream* of the parse.

`gSettings.bondPeerMac` is display-form (`formatMacAddress` → `"%02X:%02X:..."` = 17 chars), confirmed at [:14005](components/hardwareone/System_ESPNow.cpp#L14005). 17 > 14 SSO → `String cleanMac = macStr` at [:7059](components/hardwareone/System_ESPNow.cpp#L7059) allocates `(17+16)&~0xf` = **32 B, internal DRAM, 100×/sec**.

**Cost:** 1 alloc + 1 free of 32 B per tick. **Conditional:** bond master + peer online; all three gates default off (`System_Settings.h:225-227`).

**Fix:** cache the parsed 6 bytes when `bondPeerMac` is written; the tick memcpys them. Or add a `bool parseMacAddress(const char*, uint8_t[6])` overload with no String at all — kills the churn at all ~20 call sites including [:8333](components/hardwareone/System_ESPNow.cpp#L8333). The internal `String cleanMac` copy exists only to uppercase/normalize separators, and `strtol` already accepts lowercase hex — it is avoidable outright.

**Honest severity:** same-size alloc/free with nothing interleaved. Allocator CPU, not fragmentation.

#### [System_Automation.cpp:3684](components/hardwareone/System_Automation.cpp#L3684) — `schedulerTickMinute` file read · **CONFIRMED**

```c
String json;
if (!readText(AUTOMATIONS_JSON_FILE, json)) return;
```

See short-list #5. Byte-at-a-time growth of a multi-KB doc. **Cost:** ~N/16 reallocs (~256 for 4 KB), all internal DRAM. **Corrected tier:** 1×/60 s guaranteed floor, up to 4/sec with a subscribed event automation — **not** per-loop-pass. Both gates default on (`ENABLE_AUTOMATION 1`, `automationsEnabled(true)`).

### per-packet

#### [System_ESPNow.cpp:8136](components/hardwareone/System_ESPNow.cpp#L8136) — stream queue drain · **CONFIRMED**

```c
String devName = String(entry.deviceName);
if (devName.length() == 0) devName = formatMacAddress(entry.srcMac);
```

Producer is `v4h_stream` ([:3805](components/hardwareone/System_ESPNow.cpp#L3805)), registered with `flags=0` — **no pairing gate**, so unpaired peers' frames reach it. STREAM is dedup-exempt, so every frame enqueues; the drain pops up to `streamDrainMax` (64 on PSRAM boards) per tick.

**Cost:** 32 B alloc+free **per frame, only when the name >14 chars.** Unpaired/unnamed peer → `ctx.deviceName` is set to a 17-char MAC at [:4948](components/hardwareone/System_ESPNow.cpp#L4948) → always allocates. Default `HardwareOne` (11 chars) → **free**. Bursts ~24 frames in ~150 ms per remote command's output.

**Fix:** `entry.deviceName` is already a NUL-terminated `char[32]` and every consumer here takes `const char*`. Use `const char* devName = entry.deviceName;` plus a `char macBuf[18]` + `formatMacAddressBuf()` fallback — the exact pattern the RX path already uses at [:4929](components/hardwareone/System_ESPNow.cpp#L4929). Consistency fix as much as an allocation fix.

**Note:** the `length() == 0` fallback branch is effectively dead — `ctx.deviceName` is never empty. The allocation is the constructor on the line above.

### per-command

#### [System_Command.cpp:103](components/hardwareone/System_Command.cpp#L103) — `findCommand` · **CONFIRMED** (2 lanes)
Short-list #3. **Cost:** ~263 malloc/free pairs of 16–32 B per walk, ×2 walks = ~526/dispatch. Peak resident ~32 B.

#### [System_Utils.cpp:4069](components/hardwareone/System_Utils.cpp#L4069) — `authorizeCommand` redundant walk · **CONFIRMED**
Short-list #4. **Cost:** exactly #3's cost, incurred a second time, for zero information gain.

#### [System_User.cpp:276](components/hardwareone/System_User.cpp#L276) — `isAdminUser` · **CONFIRMED**
Short-list #2. **Cost:** `~filesize/16` + 2 reallocs per call, ~1–4 KB held. The largest fragmentation source in the auth lane.

**Verifier caveat worth carrying:** `resolveRole()` short-circuits to `ANON` on empty `ctx.user` ([:1450](components/hardwareone/System_Filesystem.cpp#L1450)) and `SYSTEM` for `SOURCE_INTERNAL`+`"system"` ([:1451](components/hardwareone/System_Filesystem.cpp#L1451)) *before* `isAdminUser` — which is also why the recursive `existsGuarded` on :274 doesn't infinitely recurse. So drivers (2)/(3) apply to real logged-in non-system contexts (the web/BLE listing path), not to system-internal ESP-NOW streaming. Driver (1) is unaffected: `ExecIdentityGuard` has no empty-user guard, so even anonymous commands pay the full file read.

#### [System_Utils.cpp:1146](components/hardwareone/System_Utils.cpp#L1146) — `redactCmdForAudit` · **CONFIRMED** (3 lanes)
Short-list #8. **Cost:** 2 copies × 2–3 calls per command. **Conditional on line >14 chars** — bare `help`/`ps`/`reboot` cost zero.

Two mechanism corrections from the verifiers, worth carrying so they don't propagate: (a) the claimed third copy from *"NRVO defeated by multiple divergent returns"* is **wrong** — `String(String&&)` exists at `WString.h:69`, so `return c;` is an implicit move regardless. The 2–3 count still lands, but via the `const char*` → `const String&` temporary at :921, not the return. (b) `toLowerCase()` is in-place — it adds no allocation, it just fails to rescue the copy.

#### [System_Utils.cpp:1181](components/hardwareone/System_Utils.cpp#L1181) — `redactOutputForLog` · **CONFIRMED**

```c
String redactOutputForLog(const String& output) {
  String result = output;
  // Redact password hashes: "password":"HASH:xxxxx" -> "password":"***"
```

Unconditional deep copy of the whole command output *before* probing for `"password"`/`"sid"`. Output is capped at 4095 B (`ExecReq::out[4096]`), so up to ~4 KB of internal DRAM per command; it can never reach PSRAM.

**Cost:** 1 full-output copy per call, ×1–N (the batch endpoint calls it once per command in the loop at [WebServer_Server.cpp:4786](components/hardwareone/WebServer_Server.cpp#L4786)).

**Fix — and note the correction:** an early-return guard alone does *not* help, because both callers keep `out` alive past the call, so the by-value return contract forces a copy anyway. The real fix is an API change to in-place mutation: `void redactOutputInPlace(String&)`, or `bool redactOutputForLog(const String& in, String& out)` returning false = "use the input as-is". No compat needed.

**Flagged in passing, outside this audit's scope but confirmed by a verifier:** [WebServer_Server.cpp:4811](components/hardwareone/WebServer_Server.cpp#L4811) pushes the **unredacted** `out` into the results array returned to the HTTP client, while only the broadcast sink at :4810 gets the redacted copy. That looks like a redaction bypass on the batch endpoint response body. Worth a separate look.

#### [WebServer_Server.cpp:3069](components/hardwareone/WebServer_Server.cpp#L3069) — `handleCLICommand` · **CONFIRMED**

```c
body = String(buf.get());
...
String cmdEncoded = extractFormField(body, "cmd");
String validateStr = extractFormField(body, "validate");
String captureStr = extractFormField(body, "capture");
```

The body is **already in a PSRAM buffer** (`ps_alloc "http.cli.exec"` at :3057, which uses `heap_caps_malloc(MALLOC_CAP_SPIRAM)` and genuinely bypasses the 16 KB threshold) — then immediately copied into an internal-DRAM String so `extractFormField` can be called on it. Each of the three calls re-walks the whole body allocating a substring per `&`-pair.

**Cost:** ~5 internal-DRAM allocs (not the ~9 claimed — `pk` and the `"1"`/`""` values are all ≤14 chars and SSO-free). The page never sends a `validate` field at all, so :3076 is a guaranteed full-body miss that still re-allocates the large `cmd=...` pair.

**Fix:** parse fields straight out of the PSRAM `buf.get()` with a `char*` extractor, single pass for all three. `validate`/`capture` are only compared against `"1"`/`"true"` — they never need to be Strings.

**Corrected driver:** *not* the CLI page (which polls `/api/cli/logs`, a different endpoint). But it is not purely human-typed either: [WebPage_ESPNow.h:2190](components/hardwareone/WebPage_ESPNow.h#L2190) polls `espnowmeshstatus` via `/api/cli` every 10 s while that page is open. Rate-limited to 20/sec at :3048.

#### [System_Filesystem.cpp:364](components/hardwareone/System_Filesystem.cpp#L364) — `buildFilesListing` · **CONFIRMED**
Short-list #6. **Cost:** ~95–125 reallocs, ~1.5 KB final. Plus per-entry transient Strings (`fileName`, `subPath`, `/system/settings.json` = 21 chars) the original claim didn't even count.

#### [System_Filesystem.cpp:516](components/hardwareone/System_Filesystem.cpp#L516) — `buildFilesListJson` double buffer · **CONFIRMED**

```c
out  = "{\"success\":true,\"dirPerms\":";
out += (int)dp;
out += ",\"files\":[";
out += body;
out += "]}";
```

**Cost:** ~3 allocs (not ~5) and a **2× peak** — `body` (~1.8 KB) and `out` (~1.9 KB) live simultaneously ≈ 3.7 KB internal DRAM for one listing. **Fix:** covered by #6.

Note: `filesListingJsonForApp` holds a `static String s_listJson` whose capacity persists, so the CLI/BLE path re-grows less than the web path's fresh local.

#### [System_LLM.cpp:1441](components/hardwareone/System_LLM.cpp#L1441) / [:1340](components/hardwareone/System_LLM.cpp#L1340) — `llmGenerate` transients · **CONFIRMED** (below the line)

```c
size_t prompt_buf_n = strlen(prompt) + 3;
int* prompt_tokens = (int*)malloc(prompt_buf_n * sizeof(int));
```
```c
char* norm_prompt = strdup(prompt);
```

Both verifiers found the submitted line numbers ~79 stale (quoted code verbatim correct). Both held on mechanism and arena, both flagged the cost as **20–40× overstated for the common case**: `chatBeginTurn` passes the user prompt with no history concat and no system prompt, so a real turn is ~20–40 chars → ~92 B, not ~4 KB. `prompt` is capped at 1024 by `LLMAsyncContext::prompt[1024]`.

**Fix if touched:** `EXT_RAM_BSS_ATTR static int gPromptTokens[1027];` and `static char gNormPrompt[1025];` — the PSRAM-static treatment already applied to `confHoldBuf` and `gLLMAsyncCtx` in the same file. Single-flight is already guaranteed by the `runState != READY` gate. Better still: normalize `gLLMAsyncCtx.prompt` in place (already a PSRAM-resident writable copy the worker owns exclusively) and skip the duplicate entirely.

**Also noted:** `norm_prompt` is only needed through `encode()` at :1448 but is held to :2396 — the whole generation — for no functional reason.

### per-event

#### [System_Filesystem.cpp:1702](components/hardwareone/System_Filesystem.cpp#L1702) — `appendLineWithCap` · **CONFIRMED** (2 lanes)

```c
File a = VFS::open(String(dest), "a", true);
...
File r = VFS::open(String(dest), "r");
```

Both opens are on the non-rotating fast path — the `sz <= capBytes` early-return is at :1722, *after* the second open — so every appended line pays both. `dest` is already a `char[128]` from `resolveOverflowPath`.

**Cost:** 2 call-site temporaries + 4 normalize copies inside `VFS::open` = **6 internal-DRAM allocs per log line**, 32 B each for the 27-char paths.

One arithmetic correction: `(27+16)&~0xf` = **32 B, not 48**. (48 B is right for the 34-char `system-events.log`, a different lane.)

**Three drivers, all traced:**
1. [System_Utils.cpp:937](components/hardwareone/System_Utils.cpp#L937) command-audit — every executed command, every transport. Gated only by `gCLIValidateOnly` and a 3-entry quiet-poll skiplist. The comment at :939-948 documents that an earlier debug-gated version was **deliberately un-gated** as wrong for operational output.
2. [System_Debug.cpp:305](components/hardwareone/System_Debug.cpp#L305) `[EVLOG]` tee — driven by `systemEventLogTick()` from the real main loop at [HardwareOne.cpp:2231](components/hardwareone/HardwareOne.cpp#L2231), 2 s drain. Gated on `gSettings.eventLogEnabled`, which **defaults true** (`System_Settings.h:286`) — this driver came within one default value of dying.
3. [System_Debug.cpp:281](components/hardwareone/System_Debug.cpp#L281) `[ERROR]` — real but 2 s-deduped and error-contingent. Weakest of the three.

**Fix:** `const char*` overload of `VFS::open` + the `storageTypeOfNormalized` split from #7. 6 → 0.

#### [System_VFS.cpp:402](components/hardwareone/System_VFS.cpp#L402) — `VFS::open` double-normalize · **CONFIRMED**
Short-list #7. **Cost:** 1 redundant 32–48 B alloc per FS op, ×2 per log line, ×3 on the guarded path.

#### [System_Notifications.cpp:218](components/hardwareone/System_Notifications.cpp#L218) — `notifViewerResolve` · **CONFIRMED**

```c
out.isAdmin = isAdminUser(String(username));  // live — roles change mid-session
```

This line sits **outside** the `gUserPrefsCache` gate that protects the mute mask immediately below. The surrounding code is otherwise disciplined — the per-user mute mask **is** cached, and the cache-miss path deliberately loads outside the lock. **Only the admin bit escapes the cache**, and it pays the full `isAdminUser` flash read (#2) on every resolution.

**Cost:** ~30+ allocations per call (~25 reallocs from the byte-append + ~8 substring temporaries for a 3-user, ~400 B `users.json`), up to 3× per visible event.

**Drivers:** `broadcastEventToSessionsIf` invokes the predicate once per live web session (MAX_SESSIONS=2), plus one OLED banner resolve. `SYSEVT_TEXT_RX` is rule `{ALL, 0, 2500, 0}` — **cooldown 0, unthrottled** — posted per received ESP-NOW chat message at [System_ESPNow.cpp:8609](components/hardwareone/System_ESPNow.cpp#L8609).

**Two corrections from the verifiers:**
- The "3×" is a **ceiling** requiring both session slots full AND an authed local display. Realistic steady state is 1×. The anonymous early-return at :216 zeroes it entirely when no session exists — but sessions are **TTL-based**, not page-gated (`gSessions[i].sid.length() == 0` is the only check), so a logged-in cookie keeps firing the predicate whether or not a browser is open.
- The third claimed driver (OLED notification-center rebuild) **is** page-gated and should be dropped as a steady-state driver. Its cache-gate analysis is still correct and still worth fixing (see below).

**Fix:** add `bool isAdmin;` to `UserPrefsCacheEntry` (:179-183) and populate it in the same cache-miss branch that already calls `loadUserMuteMask` (:236). Invalidation is already plumbed: `notifUserPrefsInvalidate()` flushes on every `saveUserSettings()`. Extend the invalidate call to the role-change site in `System_User.cpp` so the *"live — roles change mid-session"* intent is preserved by **explicit invalidation** rather than by re-reading flash on every event.

**Separately:** tighten the OLED rebuild gate at [OLED_Utils.cpp:218](components/hardwareone/OLED_Utils.cpp#L218) to key on the last *queue-visible* seq rather than `systemEventLatestSeq()` — the global post counter increments on **every event of every kind**, including event-only kinds that render nothing, so any chatty producer busts a view they can never appear in.

#### [System_Debug.cpp:278](components/hardwareone/System_Debug.cpp#L278) — `debugOutputTask` line assembly · **CONFIRMED**

```c
String line = buildTimestampPrefix();
line += msg->text;
appendLineWithCap(LOG_ERROR_FILE, line, LOG_ERROR_CAP);
```

`buildTimestampPrefix()` returns `"[YYYY-MM-DD HH:MM:SS.mmm] | "` = 28 chars → 32 B alloc; `+= msg->text` (≤255) reallocs to 256 B. **Cost:** 2 allocs + 1 free, plus `appendLineWithCap`'s 6. **Three identical sites**: :278 `[ERROR]`, :293 `[EVENT]`, :303 `[EVLOG]`.

**Corrected tier:** the 24-lines-per-2 s figure is a **backlog-drain ceiling**, not a rate. Every event in the X-macro list is edge-triggered; there is no polling producer. Idle device → zero lines.

**Fix:** `buildTimestampPrefix` already fills a `char tsPrefix[48]` before wrapping it in a String — expose the char form, compose into one `char line[DEBUG_MSG_SIZE + 48]` with snprintf, add an `appendLineWithCap(const char* path, const char* line, size_t len, size_t cap)` overload. Fixes all three sites.

#### [System_Automation.cpp:2414](components/hardwareone/System_Automation.cpp#L2414) — `rescheduleAfterFire` · **CONFIRMED**
Short-list #5. **Cost:** two full-document internal-DRAM growth cycles per clock fire (read + `serializeJsonPretty`), plus the flash write.

**Fix:** pass the caller's `json` in (`rescheduleAfterFire` is only ever called from `schedulerTickMinute`, which holds it). Serialize straight to the file via a Print-based writer. Dropping `serializeJsonPretty` for compact `serializeJson` also cuts document size and flash wear.

**Bug found in passing, not an allocation issue:** `nextFire()` at [:2282](components/hardwareone/System_Automation.cpp#L2282) returns `from + intervalMs/1000` — **integer division with no minimum floor** (validation at :1083 is only `isNumeric`). Any `intervalMs < 1000` yields `+0` → `nextAt == firedAt` → permanently due → this whole read+parse+serialize+flash-write runs on **every main-loop pass**. Both verifiers independently flagged this. Worth fixing regardless of the heap work.

---

## Disputed findings — one lens refuted

**33 findings.** In **24 of 33** the arena lens held (the allocation is real and is internal DRAM) and the **hotness lens refuted** (it does not run in steady state). The pattern is overwhelmingly consistent and worth naming:

> **The dominant failure mode was citing a browser-side `setInterval` as a firmware steady-state driver.**

A `setInterval(refresh, 5000)` inside an `R"JS(...)"` blob is real code, and it is genuinely periodic — in the *client's browser*, only while a human has that tab open. An idle device executes those handlers zero times. These are on-demand paths.

### Representative disputes

| Site | Arena lens | Hotness lens | Resolution |
|---|---|---|---|
| [WebPage_Maps.cpp:567](components/hardwareone/WebPage_Maps.cpp#L567) `handleWaypointsAPI` | Holds — `PSRAM_JSON_DOC` correctly PSRAM-backs the doc, then `String response` undoes it in internal DRAM; ~15–280 reallocs | Refutes — `setInterval(loadWaypoints, 5000)` is page JS, **and** `loadWaypoints()` early-returns at `WebPage_Maps.h:2050` on `!currentMap`, so the default open-page state issues **zero** requests | Real defect, doubly gated. `reserve(measureJson(doc)+1)` if touched. |
| [WebServer_Server.cpp:4838](components/hardwareone/WebServer_Server.cpp#L4838) `handleCliBatch` | Holds — `Writer<::String>` flushes every 31 B, forcing a realloc per flush; ~200 for 6 KB | Refutes — the ESP-NOW page has **7** `setInterval` timers and **none** call `refreshStatusBatch`; they poll `/api/cli`, a different handler | Tier was also per-*request*, not per-command — serialize runs once per batch, outside the loop. ~8× overstated. |
| [Bluetooth.cpp:1217](components/hardwareone/Bluetooth.cpp#L1217) `bleRawNotify` | Holds — 4 × 240 B copies per frame through the vendored `setValue`/`getValue` wrapper; verified `log_v` is compiled out at `ARDUHAL_LOG_LEVEL=1`, leaving exactly 3 live `getValue()` copies | Refutes — the claimed ~40 fps console-mirror driver requires `outble 1`, which is **off at boot** (`gOutputFlags = MSG_ROUTE_SERIAL`) and **re-cleared on every disconnect**. `Bluetooth.cpp:584-590` documents this as deliberate. | Real, per-fragment during multi-fragment results. Not the sustained 160 alloc/sec claimed. See prior-audit reconciliation. |
| [System_Automation.cpp:520](components/hardwareone/System_Automation.cpp#L520) `rebuildAutoCache` | Holds — second full byte-append read per tick | **Refutes, and inverts the claim** — the finding says *"there is no dirty check"*. There is: `HardwareOne.cpp:2211`. And `rebuildAutoCache` **populates the very cache** that prevents per-loop reads; deleting it as "redundant" would leave `gAutoCacheValid` false and force a full tick every pass — **strictly worse**. | The double-read *within a tick that runs* is real (short-list #5). The framing was backwards. |
| [WebServer_Utils.cpp:321](components/hardwareone/WebServer_Utils.cpp#L321) `getCookieSID` | Holds — 32-char token → 48 B, and the 512 B parse buffer is correctly excluded as `ps_alloc` PSRAM | Refutes — the cited 2 Hz driver (`handleLogs`) calls `getCookieSID` **once**, not twice; `/api/notice`, the most poll-shaped double-caller, has **zero fetchers anywhere in the tree** | Real double-call exists on `handleSensorsStatusWithUpdates` (1 Hz, page-gated). The headline driver was wrong. |
| [HardwareOne.cpp:741](components/hardwareone/HardwareOne.cpp#L741) 4 KB capture buffer | Holds — plain `malloc(4096)`, under 16384 → internal DRAM, deliberately heap "to avoid blowing cmd_exec_task stack" | Refutes — `captureOutput` has exactly one true-producer (`capture=1`), and **no in-repo client sends it**; the only caller is `WebPage_LLM.h:531`, a manual Run button that self-disables after one click | Not per-command. External-API-driven (the OpenClaw bridge). |

### Where the arena lens refuted instead

Only a handful. The most instructive:

- **[System_Microphone.cpp:433](components/hardwareone/System_Microphone.cpp#L433) / [:458](components/hardwareone/System_Microphone.cpp#L458)** — the `seen +=` dedupe loop is a genuine textbook instance of mechanic #3. But `ENABLE_MICROPHONE_SENSOR` is **0 on FeatherS3** (verified at `System_BuildConfig.h:168-174`: it requires `ARDUINO_XIAO_ESP32S3_SENSE_DEV`). The whole file body sits behind `#if`. The linked symbol is the stub `inline String getRecordingsList() { return "[]"; }`. **And the claimed driver is itself compiled out** — `System_Microphone_Web.h` is gated on the same macro, so the `setInterval(loadMicRecordings, 5000)` is never emitted into the served page. The auditor cited a real source line without checking whether it compiles.

---

## REFUTED appendix — checked and dismissed

This is signal, not noise: it tells you what a future audit should not re-raise, and it exposes two fabricated citations that would otherwise propagate.

**1. All of `System_MQTT.cpp` — `ENABLE_MQTT` is 0.**
Four findings (`:106`, `:336`, `:719`, `:770`) were dismissed. `System_BuildConfig.h:103` defines `ENABLE_MQTT 0` at top level, and the entire file body is wrapped in `#if ENABLE_WIFI && ENABLE_MQTT` (`System_MQTT.cpp:13`). Zero cost.

> **Correction to the verification record:** one verifier claimed the source file is excluded by `CMakeLists.txt:351 if(HW_CFG_ENABLE_MQTT GREATER 0)` with a regex read at `:59-61`. **That is fabricated** — I grepped `CMakeLists.txt` for `mqtt` case-insensitively and it does not appear at all. The real mechanism is the `#if` guard: the translation unit **is** compiled, its body is empty. Same net effect, wrong mechanism. Do not propagate the CMake line.

**2. `System_Microphone.cpp` — not compiled on the primary board.** See above.

**3. `System_ESPNow.cpp:8333` post-sync fallback parse — the "indefinite 100 Hz" state is unreachable.**
The claim rests on `bondStatusReqSentOnce` only being settable via `firePostSyncSideEffects` gated behind `bondSessionActiveWith(pMac)`. **False:** `firePostSyncSideEffects` has three call sites, and the primary one — `processBondSettings` at `:6621` — calls it with **no session check at all**. Sync completion *is* the arrival of the encrypted settings, so the session is ACTIVE at that instant and the latch sets there. In steady state the guard short-circuits before the parse. The pathological window is additionally hard-bounded at 15 s by `BOND_HEARTBEAT_TIMEOUT_MS` → `resetBondSync()` → `isBondSynced()` false. Bounded transient, not steady state.

**4. `WebPage_Bond.cpp:1121` — three of the five `getCapabilityListLong` calls are gated.**
Calls 3–5 at `:1155-1157` sit behind `if (gEspNow && gEspNow->lastRemoteCapValid)`. The claim's "~35-50 reallocs per poll" also over-reads the name tables as if every row concatenates; the mask selects only the compiled-in subset (5 of 10 features, 4 of 9 sensors on FeatherS3), giving ~7 reallocs. ~5× overstated on a page-gated 5 s poll.

**5. `OLED_Mode_Network.cpp:821` — "8 Hz per-frame" is not a rate.**
`updateOLEDDisplay()` returns early on `!oledIsDirty()` *before* the mode switch. `oledIsDirty()` is edge-triggered only (input seq, sensor seq, force-render, pairing ribbon). The verifier found the corroborating in-tree evidence: `i2csensor_ano_encoder.cpp:636-644` documents that an unconditional per-poll `gInputCache.seq` bump was a **fixed bug** because it "made gInputCache.seq advance ~33x/s forever". Passively viewing the page yields **zero** calls after the first render.

**6. `System_SensorLogging.cpp:504` — double-gated off by default.**
`gSensorLoggingEnabled = false` (`:60`) returns at `:74` *before* the interval check the claim quotes at `:78`; `sensorLogAutoStart()` returns at `:1063` on `gSettings.sensorLogAutoStart`, default false. Three findings cited the **100 ms setting floor** as the operating rate; the default is **5000 ms**. Even opted-in that is 0.2 Hz — a 50× overstatement.

---

## Systemic fixes — these beat site-by-site patching

Site-by-site patching of the findings above would take a dozen commits and miss the next instance. These four structural changes subsume most of them.

### S1. Stop `readText()` reading every file one byte at a time

[System_Utils.cpp:804](components/hardwareone/System_Utils.cpp#L804).

Because `fs::File` inherits `Stream::readString`'s per-byte `ret += (char)c`, **every whole-file read in this
firmware** costs ~N/16 relocating reallocs. That much is **confirmed by direct code reading** and is the most
interesting mechanical finding in the audit — nobody wrote that loop, it was inherited from a vendored base
class three call levels down.

> ⚠️ **CORRECTED 2026-07-16.** This section previously read "`out.reserve(f.size() + 1);` before
> `out = f.readString();` … Do this first. It is the highest-value line in the document." **That fix does not
> work** — the churn is inside `readString`'s own local String, and reserving `out` forces `String::move()`
> onto its memmove branch instead of a buffer steal, making it strictly worse. See the full correction in
> short-list item 1.

The fix must **bypass `Stream::readString` entirely** — `reserve()` on `out` is not a lever here. Either
chunk-read via `f.read()` into a reserved `out` (see item 1), or for the large-document callers read into a
`ps_alloc` PSRAM buffer and parse in place, keeping the whole document out of internal DRAM at all.

**Priority caveat — read `HOT_PATH_HEAP_AUDIT_MECHANICS.md` before ranking this first.** The mechanics
analyst ranks `reserve()`/String discipline **5th**, argues it is "a lot of diff for a fraction of the
benefit," and concludes String churn is *the stirrer, not the killer*. This fix is cheap and correct, but it
is probably **not** the highest-value change in the codebase. That reconciliation is unfinished.

### S2. `const char*` overloads across the VFS/log seam

The pattern repeats identically in at least five places: code carefully builds a `char[128]` with snprintf, then throws it onto the heap to satisfy a `const String&` parameter, then the callee immediately calls `.c_str()`.

- `VFS::open` / `openGuarded` currently take `const String&` only (`System_VFS.h:115`, `:145`) — callers pass `char dest[128]` (`appendLineWithCap`), `char activePath[128]` (`sensorLogTick`).
- `appendLineWithCap` takes `const String&` — callers build the line from a `char[48]` prefix.
- `redactCmdForAudit` takes `const String&` — `logCommandExecution` passes a `const char*`, materializing a temporary just to bind.
- `parseMacAddress` takes `const String&` — ~20 call sites, several on tick loops.
- `getEspNowDeviceName` wraps a `char[32]` from the peer table in a String purely to return it.

**No backwards compatibility is needed here.** Don't add overloads and leave the String versions to rot — **change the signatures** and fix the call sites. That is the clean break this repo is allowed to make.

### S3. Split `getStorageType` from `normalize`

[System_VFS.cpp:315](components/hardwareone/System_VFS.cpp#L315). Add `static StorageType storageTypeOfNormalized(const String& p)` doing just the `/sd` prefix test with no copy; call it from `open`/`exists`/`mkdir`/`remove`/`rename`/`rmdir` where `p` is already normalized. Keep the public `getStorageType` as a normalize-then-delegate wrapper. **Zero behavior change**, removes one internal-DRAM alloc from every FS op in the firmware. Combined with S2 this takes `appendLineWithCap`'s fast path from 6 allocs per log line to 0.

### S4. Stop serializing PSRAM JsonDocuments into internal-DRAM Strings

`PSRAM_JSON_DOC(doc)` correctly places the document in PSRAM. Then `String out; serializeJson(doc, out);` materializes the entire payload back in the scarce heap — through ArduinoJson's `Writer<::String>`, which flushes a 32-byte buffer via `String::concat` every 31 bytes, and (per mechanic #3) **every one of those flushes forces a realloc**. An N-byte document costs ~N/31 reallocs. The PSRAM document buys nothing.

The correct idiom already exists in-tree at `WebServer_MigrationTool.cpp:337-342`:

```c
size_t n = measureJson(doc);
auto out = ps_alloc(n + 1, AllocPref::PreferPSRAM, "tag");
serializeJson(doc, out, n + 1);
httpd_resp_send(req, out, n);
```

`WebPage_Battery.cpp:33` uses the stack-buffer variant for bounded payloads. `gJsonResponseBuffer` (`WebServer_Server.cpp:2514/2559/2628`) and `sysJsonBuf` (`:3030`) are the same idea. **The pattern is right there — the offending sites just don't follow it.** Most are on user-driven paths (hence DISPUTED), but this is one grep-and-replace that closes the whole class permanently, and the minimum viable fix per site is a one-liner: `response.reserve(measureJson(doc) + 1);`.

### S5 (smaller, but structural): honor the caches this codebase already built

Twice, the firmware builds exactly the right cache and then a neighbor ignores it:

- `ExecIdentityGuard` memoizes `isAdmin` into task TLS; `resolveRole()` hits flash anyway.
- `gUserPrefsCache` caches the mute mask; the admin bit on the very next line escapes it.

Neither needs new machinery — `gIdentityGeneration` and `notifUserPrefsInvalidate()` are already built, documented, and correct. This is a discipline problem, not a design problem.

---

## Reconciliation with `docs/PRE_1_0_CODE_HEALTH_AUDIT.md` (2026-07-14)

That audit's **section 1a** covered similar ground. Its own preamble warns that only findings marked ✓verified were re-read, and that its verifier pass was dropped for spend. This audit two-lens-verified the overlap. **The reconciliation is not flattering to either document, and both directions matter.**

### CORRECTED — its #1 short-list item is now stale

Prior audit item 1: *"Kill the per-command waste in `executeCommand` — a **dead `String lc` copy + `toLowerCase()` that is never read** ([:4272](components/hardwareone/System_Utils.cpp#L4272)), **unconditionally copies the full command line** for the rare `remote:`/`@` case ([:4180](components/hardwareone/System_Utils.cpp#L4180)), and **rebuilds a `normalizedCmd` String every call** solely to feed two default-off debug lines ([:4303](components/hardwareone/System_Utils.cpp#L4303)). Three heap allocations per command."*

I checked all three against the current tree:

- **`normalizedCmd` does not exist.** `grep -n "normalizedCmd" System_Utils.cpp` → zero hits.
- **The dead `String lc` in `executeCommand` is gone.** The only `String lc` in the file is at `:4051`, inside `adminRequiredForLine` — a different function, where it is genuinely read.
- **The unconditional command-line copy is now deliberate and documented.** `System_Utils.cpp:4194` reads:
  ```c
  // Create command String once — reuse everywhere (avoids 5+ String(cmd) temporaries)
  String command = cmd;
  ```

Someone acted on that item between 2026-07-14 and now. **Item 1 of the prior short list is done. Do not re-do it.** Its line numbers have drifted ~90 lines and should not be trusted for navigation.

### REFUTED — its #3 (BLE notify) is real but its frequency claim is not

Prior audit item 3 / `Bluetooth.cpp:1191`: *"a paced secure-channel file read (~33 frames/s at 225 B) churns ~30 KB/s of internal DRAM through this path."*

The mechanism is **confirmed** and if anything under-counted: `setValue` copies once, `notify()` calls `m_value.getValue()` three times by value, and this audit additionally verified that `log_v` is compiled out at `CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL=1` so exactly 3 live copies survive — 4 × 240 B per frame, up to 19 under congestion retries.

But **~30 KB/s sustained is not a steady state this firmware reaches.** The console-mirror driver requires `outble 1`; `gOutputFlags` is initialized to `MSG_ROUTE_SERIAL` at `HardwareOne.cpp:444`, the bit is set in exactly one place (`cmd_outble`), it is re-cleared on every disconnect, and `System_Settings.cpp:727` masks-and-preserves but never *sets* it, so it cannot survive a reboot. `Bluetooth.cpp:584-590` documents this as deliberate design. The residual real path is per-command results — bursty, mostly single-fragment.

**Corrected:** real per-fragment copy, DISPUTED tier, worth fixing as a burst cost during transfers. Not a 30 KB/s steady-state drain.

### CORRECTED — its `HardwareOne.cpp:753` (4 KB capture buffer) is narrower than stated

Prior audit: *"all web CLI capture requests."* This audit traced `captureOutput` to exactly one true-producer (`capture=1` at `WebServer_Server.cpp:3110`) and found **no in-repo client sends it**. The only in-tree consumer is `WebPage_LLM.h:531` — a manual "Run" button for an LLM-suggested `Do:` command that self-disables after one click. The general web CLI console does **not** send `capture=1`.

Real driver is an external API caller (the OpenClaw `/api/cli` bridge). Also: the malloc/free pair opens and closes within one iteration of a single-threaded worker, so the allocator hands back the same block each time — a transient high-water bump, not a fragmentation driver. **Downgraded to DISPUTED.**

### CONFIRMED — its #5 (debug-layer String copy)

`System_Debug.cpp:278` is confirmed by both lenses here, with the tier corrected to **edge-triggered per-event** (the 24-lines/2 s figure is a backlog ceiling; idle devices emit zero). The `:834` half (`broadcastOutputCore` → `sendEspNowStreamMessage(String(text))`) is confirmed on mechanism but **DISPUTED on tier**: `gCurrentStreamCmdId` is set at `System_ESPNow.cpp:5260` and cleared at `:5030` when the command completes — a window of **milliseconds**, not the 30 s `STREAM_SESSION_TIMEOUT_MS` the finding assumed (that constant governs slot reuse in `gStreamSessions[4]`, not this global).

### NOT REVISITED

Items 2 (thermal/ToF busy-spin) and 4 (delete `BLE_IDF.cpp`) are out of scope here — neither is an allocation finding. Item 2 is a real CPU bug and its ✓verified status looks sound; it should still be fixed.

### THE HEADLINE — what the prior audit missed

The prior audit devoted its **#1 short-list slot** to three allocations per command in `executeCommand`, two of which no longer exist.

It did not find that **`findCommand` walks a 1222-entry registry with no early exit, constructing a heap String from 263 of those names, twice per command dispatch** — ~526 malloc/free round-trips and ~3772 string ops on the universal chokepoint. It did not find that **`isAdminUser` reads `users.json` off flash on every command, every guarded FS op, and every directory entry**. And it did not find that **`readText` reads every file one byte at a time**, which is the actual engine behind the fragmentation it was looking for.

Those three are items 1–4 of this audit's short list. The lesson is in the prior audit's own preamble: it ran 28 auditors and dropped the verification pass. **The findings that survive adversarial verification are not the ones that look worst on inspection.** A four-line `String lc` copy is visible; a missing `reserve()` inside a vendored `Stream` method three call levels down is not.

---

## Appendix: what to actually do, in order

1. **`out.reserve(f.size() + 1)` in `readText`** — one line, fixes the engine (S1).
2. **Cache the admin verdict against `gIdentityGeneration`** — kills the biggest per-command file read (#2).
3. **Hoist `findCommand` in `executeCommand`, then delete its inner String** — halves, then zeroes, the dispatch cost (#3, #4).
4. **Pass the tick's `json` into `rebuildAutoCache` / `rescheduleAfterFire`** — removes 1–2 full-file reads per tick (#5).
5. **`const char*` through the VFS seam + split `getStorageType`** — 6 allocs per log line → 0 (S2, S3).
6. **`reserve()` / `measureJson` the serializeJson-into-String sites** — one grep, closes the class (S4).
7. **`buildFilesListing`: `reserve(2048)` + snprintf per entry + delete the `body` double-buffer** (#6).
8. **`redactCmdForAudit` / `redactOutputForLog`: match before copying; change the signatures** (#8).

Then stop. Everything else in this document is either below the line or DISPUTED, and the reader's time is better spent on the `nextFire()` integer-division bug (`System_Automation.cpp:2282`) and the batch-endpoint redaction bypass (`WebServer_Server.cpp:4811`) that this audit tripped over on the way.
