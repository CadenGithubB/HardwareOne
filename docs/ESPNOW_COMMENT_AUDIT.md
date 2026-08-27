# ESP-NOW Comment Truthfulness Audit

> **STATUS 2026-07-16 (later): the comment corrections are DONE and building green.**
> 65 comments corrected across 19 files; 6 findings rejected after verification (see §13).
> **Proven comment-only:** code compared with comments stripped — byte-identical across all 31 files.
> **Proven zero-cost:** an A/B build with the pre-fix comments restored produced a byte-identical
> binary (`0x4ecd80`), so the corrections cost nothing.
> The **code** bugs in §2/§3 are deliberately NOT fixed — they change behavior and are listed in §14.

**Date:** 2026-07-16
**Scope:** the whole ESP-NOW system — ~34,000 lines across 21 files (`System_ESPNow*.{cpp,h}`, `System_BondedPeer.*`, `OLED_ESPNow.cpp`, `G2_Page_ESPNow.cpp`, `WebPage_ESPNow.{cpp,h}`).
**Method:** 8 parallel auditors, one per file group. Each was required to verify every claim against real constants, struct definitions and grep — never against another comment — and was forbidden from using `git diff`/`HEAD` as evidence (the tree carries heavy uncommitted WIP). Struct sizes were independently recompiled, not taken on faith.
**Mandate:** find comments that are FALSE about the code. Not code review, not bug hunting, not missing comments.

**Result: 93 findings — 22 HIGH, 46 MEDIUM, 25 LOW.** No files were edited.

> The code is fine. This audit says nothing about whether ESP-NOW works — it demonstrably does. It says the **map** is wrong in ways that would mislead the next person to touch it, including the several places where following the comment reintroduces a bug the repo already fixed.

---

## 1. The five failure modes

Every finding fits one of these. They're worth naming because they predict where to distrust the file next time.

1. **Stale arithmetic after a constant moved.** `MESSAGES_PER_DEVICE` went 100→250 and the allocation was updated; the prose wasn't. Everything downstream that multiplies by it is now 2.5–6.5× low.
2. **The 4× stack myth, promoted to architectural justification.** `System_TaskUtils.h` was corrected in place (*"VALUES ARE BYTES… HISTORICAL FIGURES BELOW ARE 4x INFLATED"*), but the inflated numbers had already been copied into other files' *reasoning*, where they now justify design decisions.
3. **Tense drift — aspiration written as history.** "Removed in Phase 5" (still live at 8 sites), "Future kinds: JOB_RAW" (dispatched 20 lines below), "that lands in Phase 3" (landed, in the same function).
4. **Model inversion.** The code's architecture changed and four+ comments still narrate the old one (TEXT per-fragment storage, tick-driven FsList replies, `gStreamFile` threading).
5. **Cross-file duplication drift.** A fact restated away from the code it describes rots. Every wrong line-number cross-reference checked (5 of 5) was stale.

**The single most useful signal:** *these files usually contradict themselves, and the copy nearer the code is the true one.* Examples: `static_assert(==164)` vs "Reply is 172 B" (24 lines apart); `static_assert(<=208)` vs "well under 128 B" (79 lines); `Files.h:32` correct vs `Files.h:96` wrong (60 lines); `Tx.h:33` "Removed" vs `Tx.h:129` "DEADLOCK RULE" presupposing it exists. Headers and section banners lie; inline comments next to the code are reliable.

---

## 2. Findings that are real bugs, not just wrong prose

These escape the "comments only" frame. Each is code, or a decision made from a false comment.

### 2.1 The pre-flight memory gate is sized from a stale comment — HIGH
`System_ESPNow.cpp:9241` says initEspNow is *"~360 KB alloc"*. Real cost at default `meshPeerMax=8` is **~855 KB** (message history alone is 5 × 162,016 = 810 KB). The 360 KB figure is arithmetic from the pre-100→250 era.

**The bug:** `System_MemoryMonitor.cpp:84` admits ESP-NOW when **327,680 B** of PSRAM is free — 2.6× less than init needs. The guard passes, then init fails. This is the same cliff as §2.2.

### 2.2 `growPeerHistoryArray` needs 2× transiently — HIGH (no comment denies it; recorded here)
Growth allocates the new array, placement-news it, memcpys, *then* frees the old. 5→8 slots = 810 KB + 1.3 MB = **2.1 MB peak**; 13→16 ≈ 4.7 MB. With a model loaded (~7.3 MB of 8 MB) growth cannot succeed, so "grows as peers are discovered" silently doesn't hold. *(Not a comment finding — the auditor correctly declined to log it as one. It belongs in the redesign.)*

### 2.3 `V4RxCtx` is declared twice, out of sync — HIGH (ODR violation)
`System_ESPNow_Handlers_Crypto.cpp:43` re-declares `V4RxCtx` under a comment reading *"must stay byte-for-byte in sync with the original. If you change one, change both."* The copy is **missing `isAuthenticated` and `isSessionEncrypted`** — sizeof 16 vs 20. Two conflicting definitions of one type in one program.

Doesn't misfire today only because the file touches just the fields below the divergence. The first read of `ctx.deviceName` added there compiles cleanly and silently reads the auth flags. Those two dropped fields are the frame's authentication signals, whose own comment warns handlers *"MUST gate on this — otherwise anyone in radio range can spoof."*

### 2.4 The `espnowbuffers` settings are write-only — HIGH
`espnowTxQueueSize`, `espnowRxBufferSize`, `espnowChunkSize`, `espnowFileChunkSize` have **zero consumers** in the firmware. Real values are compile-time constants (`ESPNOW_V4_MAX_PAYLOAD`, and a hardcoded `const size_t FRAG = 200`). The CLI answers *"set to 150 (takes effect after reinit)"*. It never takes effect.

### 2.5 `espnowmeshes add <label> <passphrase>` silently discards the passphrase — HIGH
`System_ESPNow.cpp:11716` documents a passphrase argument. The dispatcher reads only `arg(1)`; the handler hard-sets `passphrase = ""`. `espnowmeshes add lab hunter2` succeeds, drops the secret, leaves an unencrypted mesh. Registry help and the handler's own error string document it correctly — this docblock is the lone outlier.

### 2.6 `espnowIdentityRegenerate` doesn't wipe what it says it wipes — HIGH
Requires `--confirm-wipe-all-bonds` and tells the user *"all previously paired peers must be re-paired"*. It deletes **no** peer trust record; 16 stale identities survive on disk and in RAM. The header excuses this: peer records *"don't exist until Phase 3.2."* Phase 3.2 landed **in that same file** (`writePeerIdentityFile`, `peerIdentityForget`, `peerIdentityLoadAll`, loaded at boot from `HardwareOne.cpp:1944`).

### 2.7 `sessionAllocate`'s "cleared" is false on the reuse path — HIGH
`System_ESPNow_Sessions.h:113` promises *"Existing session for the pair is replaced (cleared)"*. The reuse branch leaves `aeadKeyRxPrev[32]`, `prevKeysValidUntilMs`, `rekeyEphPrivKey[32]` and `bondToken[16]` untouched — while the sibling free-slot branch does a full `memset` and `sessionClear` fully zeroes. `sessionUnwrapFrame` will try `aeadKeyRxPrev` whenever `prevKeysValidUntilMs` is unexpired, a deadline this path preserves across a re-handshake.

---

## 3. Comments that invite reintroducing a fixed bug

The most dangerous category: not stale, **never true**, and the repo records the crash.

### 3.1 The placement-new String invariant — HIGH
`System_ESPNow.cpp:8828` claims placement-new *"runs every constructor so all Strings are valid empty SSO strings"*, naming the exact crash it prevents: *"a freshly-paired peer whose friendlyName/room/zone/tags metadata hasn't arrived yet."*

`EspNowState`'s ctor body (`System_ESPNow.h:937`) runs `memset(devices, 0, sizeof(devices))` **after** those 80 Strings are constructed, re-zeroing them to `{buff=NULL}`. `c_str()` returns NULL for exactly the named fields. The struct-level Strings (`passphrase`, `deviceName`) *are* fine, which makes the claim partially true and more convincing.

`System_ESPNow.cpp:10312` manually assigns `= ""` to those four fields, commented **"This is why a freshly-paired device crashed the bond page."** Anyone trusting :8828 deletes those as redundant and reintroduces a documented LoadProhibited crash. The same comment also says it replaced "the old blanket memset" — that memset runs on the line above it.

### 3.2 The 6143 clamp credited to the wrong constant — HIGH
`System_ESPNow.cpp:2776`: `if (resultLen > 6143) resultLen = 6143;  // V4 fragmentation budget: 32 × 200 = 6400 B`. The clamp is buffer-derived (`ps_alloc(6144)` − 1). The fragmentation budget genuinely *is* 6400 — which **exceeds the buffer**. "Correcting" 6143 → 6399 to match its own comment overflows the heap by 256 B.

---

## 4. Wire-contract falsehoods

`System_ESPNow_Wire.h` is declared the source of truth for the protocol. Two entries are wrong.

| Line | Claim | Truth | Impact |
|---|---|---|---|
| `Wire.h:426` | CONFIRM transcript = **88 bytes** | **87** (`buildConfirmTranscript(uint8_t out[87])`, final memcpy at 71+16) | An external implementer signs 88 B; **every Ed25519 signature fails**, with no diagnostic beyond "bad signature". OPEN=71 is correct, which makes it worse. |
| `Identity.h:90` | `SUBSCRIBE_UPDATE = **70**` | **130**. 70 is `FS_LIST_REQ` | Opcode-collision trap; fossil of the pre-2026-07 10-wide map |

---

## 5. The 4× stack myth, third act

`System_TaskUtils.h` already carries the correction. These files copied the *inflated* numbers into their reasoning before that sweep, and now use them to justify architecture:

| Location | Claims | Real |
|---|---|---|
| `Handlers_Crypto.cpp:716` | espnow_task "22 KB budget"; cmd_exec "24 KB"; CLI peak "17 KB" | **6.5 KB**, **8 KB**, **~4.25 KB** |
| `FsList.h:51` | "~3 KB peak — well under the 24 KB stack that task has" | 8 KB. And the buffer isn't on the stack at all — it's `ps_alloc`'d |
| `FsList.cpp:463, 902` | "espnow_task's 22 KB stack" justifies the cmd_exec handoff | 6.5 KB |

This is the whole stated rationale for running Ed25519/X25519 on cmd_exec. Someone re-deriving it from these numbers could reasonably conclude espnow_task's "22 KB" absorbs the crypto inline — against a real 6.5 KB. The decision is *correct*; its written justification is off by 4×.

Related: `Sensors.cpp:586` and `Tx.cpp:107` label `uxTaskGetStackHighWaterMark` output as **"words"**; ESP-IDF returns bytes. Arithmetic is accidentally right (both operands bytes), labels invite a 4× misread.

---

## 6. Memory arithmetic (all independently recompiled)

| Location | Claim | Truth | Factor |
|---|---|---|---|
| `System_ESPNow.h:530` | peer history initial = "~125 KB" | **810,080 B** | 6.5× |
| `System_ESPNow.h:531` | growth step = "~75 KB" | **486,048 B** | 6.5× |
| `System_ESPNow.cpp:9241` | initEspNow "~360 KB alloc" | **~855 KB** | 2.4× |
| `System_ESPNow.cpp:8924` | reasm "saves ~6.4 KB" | **12,912 B** (one entry ≠ the array) | 2× |
| `Sessions.h:18` | slot "well under 128 B", "~2 KiB total" | **208 B**, **3,328 B** | 1.6× |
| `System_ESPNow.h:500` | `ReceivedTextMessage` "~316 B" | **324 B** | seed of the 6.5× error |
| `FsList.cpp:747` | REPLY "up to ~500 B" | **2,572 B** | 5× |
| `WebPage_ESPNow.cpp:308` | sendstatus "~30 B/entry, 480 B" | 64–85 B, **1,024–1,360 B** | 2.8× |
| `Wire.h:653` | FS_STAT reply "172 B" | **164** (`static_assert(==164)` 24 lines below) | — |
| `System_ESPNow.h:770` | metadata "V4PayloadMetadata size (212)" | **202** | — |

Ceiling worth stating plainly: peer history maxes at 16 × 162,016 = **2.47 MB PSRAM**, not the ~400 KB the comments imply.

**`ESPNOW_TEXT_MAX_LEN` deserves its own line.** Its comment claims it *"sizes the receive queue slot AND the per-message history slot"* and costs *"~+600 KB PSRAM across the 100×8 history ring."* It is used at exactly two sites, both pure length **gates**. Both slots it claims to size are hardcoded `char[256]`. Changing 1024 costs **zero bytes**. "100×8" matches nothing that has ever existed alongside `MESSAGES_PER_DEVICE=250`. And its "must hold the full message" invariant is violated by `copyLen = min(payloadLen, 255)`.

---

## 7. Inverted claims — comments asserting the opposite of the code

| Location | Claim | Truth |
|---|---|---|
| `System_ESPNow.cpp:15425` | cap drops "remaining (**older**) messages" | Walk is oldest→newest; the cap drops the **newest**. Contradicts its sibling at :15471 ("callers show the tail for 'most recent'"). An inbox sized from this loses exactly what it exists to show. |
| `System_ESPNow.cpp:2527` | CMD_RESP "**falls back to plaintext** if no session is up" | `v4_send_payload_smart` is strict encrypt-or-fail; the response is dropped. Contradicted at :2082 (*"No plaintext fallback exists anywhere in this path"*). |
| `System_ESPNow.cpp:3692` | FS_LIST/STAT/GET "gated on REQ_PAIRED + **REQ_BOND_MODE** + REQ_SESSION_ENC… sender is a bonded peer" | Rows carry only `REQ_PAIRED\|REQ_SESSION_ENC`. **No bond gate** on remote filesystem access. The table's own comment at :4437 confirms de-bond-gating was deliberate. |
| `System_ESPNow.cpp:11965` | non-zero mesh slots "just persisted metadata, **not used for runtime encryption**" | `meshKeysDerive(i)` runs for every slot 8 lines below; those keys authenticate KEY_EX HMACs and sign BROADCAST_AUTH. User is told a live key is inert; a maintainer could delete the derive calls as dead. |
| `Files.cpp:82` | `gStreamFile` "opened in fileSlotsAllocate… on espnow_task… serialized by gFileSlotsMutex" | Opened/written on **cmd_exec**, deliberately **outside** the mutex. Inverts the module's core threading contract — the module exists to keep flash off espnow_task. |
| `FsList.h:150` | RX entrypoints "Called from **BTC_TASK** (tiny stack)" | BTC_TASK is the *Bluedroid BLE* task, nowhere in this path. Same .cpp says "espnow_task RX-handler context" 5×. Hands every callback author the wrong stack budget and deferral rule. |
| `Saturation.h:17` | ring is "static **DRAM**… **No PSRAM**" | `EXT_RAM_BSS_ATTR Sample gRing[30]` — it's PSRAM. Exactly backwards; charges 1.1 KB to the scarcer pool. |
| `Tx.h:23` | producers (incl. sensor_bcast) "**never touch** the AEAD seal, frame buffer, or esp_now_send" | True for bond only. sensor_bcast's mesh path calls `esp_now_send` inline (~1 KB of frame[250]+line[700] stack) against a real 4 KB budget. The belief already leaked into `SENSOR_BCAST_STACK_WORDS`' sizing note. |
| `WebPage_ESPNow.cpp:132` | bursts paginate via `?since=`, "**no data is lost**" | `getAllMessages` fills peer-by-peer then sorts only the returned subset (its own comment concedes it "can miss the true global-newest"); the client advances its cursor to the max seq seen, so capped lower-seq records fall permanently below it. |

---

## 8. Stale dispatch/index maps (sit directly on the code they misdescribe)

- **`G2_Page_ESPNow.cpp:969`** — *"2 — peers, 3 — broadcast, 4 — stats."* Real: 3=Inbox, 4=Broadcast, 5=Stats, 6=Bonded. Inbox was inserted at row 3; only row 2 still matches.
- **`OLED_ESPNow.h:84`** — documents `settingsMenuIndex` as 5 entries; there are **10**, and 4 of 5 indices are wrong (says 1=Passphrase; it's Room). This field routes `espnowsetpassphrase` and `espnowmeshmaster` writes.
- **`G2_Page_ESPNow.cpp:1152`** — describes tap→Peer Detail navigation and MAC re-resolution. The body ignores `idx` and calls `showMainMenu()`; it never touches `gEspNow`.

---

## 9. Dead-counter audits with holes in them

Two comments certify that dead counters were pruned. Both are wrong, in the same way as the `gEspNowRxDrops` case:

- **`System_ESPNow.cpp:9616`** — *"Pruned to ONLY counters that are actually live today."* `chunksTimedOut` survived, is still printed as "Reassembly Timeouts" / `"reassemblyTimeouts"`, and has **no increment site in the repo**. It reads 0 forever. (The real counter is `v4FragRxGc`, printed one line above.)
- **`gEspNowRxDrops`** — the original example. Now accurate at `:976` (*"nothing reads gEspNowRxDrops today"*), corrected earlier the same day. Two auditors independently confirmed the fix rather than parroting the brief.

---

## 10. Abandoned two-word CLI convention

The `espnow <verb>` → `espnowverb` migration reached the registry and most user-facing strings, then stopped. **Six live user-facing strings** still print the dead form: `espnow buffers`, `espnow broadcast`, `espnow browse`, `espnow fetch`, `espnow remote`, `espnow send` — plus ~9 internal docblocks (`bond connect`, `bond status`, `bond role`, …).

The file documents its own failure at `:13169`: *"the command token is `espnowsendfile` (one word)… Emitting `espnow sendfile` (two words) makes the remote reject it as `Unknown command`, which is why fetch silently failed."*

---

## 11. What is *right* — and why it matters

Recorded because it identifies what to trust, and because the auditors were told not to pad.

**Anything a compiler or `static_assert` can check is scrupulously accurate.** Every payload-size assertion, every wire field offset (all 14 header fields, 0→31), every opcode's category placement, the `CapabilitySummary` reserve-byte offsets (19/39/49), `V4PayloadMetadata` "exactly 202", the OPEN transcript = 71 — all verified exact.

Also verified true: the AAD extent (30 B, `crc16` really at offset 30–31), nonce construction byte-for-byte, `offsetof(MeshPeerMeta,isActive)==0xE0`, the RX-ring alloc genuinely preceding `esp_now_register_recv_cb`, the 100 Hz sweep, `gRemoteSensorCache` "~20 KB" (64 × 316 = 20,224), the saturation ring "~1.1 KiB" (30 × 36 = 1,080), the 2,572 B FS_LIST reply, "128 KB × 4 slots = 512 KB", `mergePeerRingsBySeq`'s "cap applied AFTER interleaving", the `EspNowTxGuard` reentrancy claim, and the whole of `System_BondedPeer.{h,cpp}` (**zero findings**).

Two auditors independently **refuted** a false example I seeded them with (a `gPeerIdentities` PSRAM/DRAM contradiction that no longer exists — I fixed it earlier the same day), one calling that comment "among the best in scope." They verified rather than confirmed.

**The pattern:** this codebase's comments are the work of someone who genuinely did the arithmetic. They fail at exactly one seam — **when a constant moves or an architecture changes, the code and its adjacent comment get updated; the header, the banner, and the cross-file restatement do not.**

---

## 12. Suggested order

1. **The two booby traps first** — §3.1 (placement-new) and §3.2 (6143 clamp). Both actively invite reintroducing a bug. Cheapest, highest value.
2. **`MemoryMonitor.cpp:84`** (§2.1) — a real gate, sized from a stale number, admitting ESP-NOW at 2.6× under its need. This is code, not a comment.
3. **`Wire.h:426`** (§4) — the CONFIRM transcript length. One character; silently breaks any external implementer.
4. **`V4RxCtx`** (§2.3) — delete the duplicate and include the real definition, or sync it. It's an ODR violation today.
5. **The user-facing lies** (§2.4, §2.5, §10) — write-only settings that claim to apply, a passphrase silently dropped, six commands printing syntax that doesn't exist.
6. **The 4× stack figures** (§5) — mechanical, but they currently *justify* an architecture.
7. **Everything else** — a mechanical sweep. Per §1, the true copy is usually already in the file; it just wasn't propagated.

**Do not** start the lazy-per-peer history redesign (`docs/LAZY_ALLOCATION_AUDIT.md` §Tier 1) until at least §2.1, §2.2 and §6 are settled — that redesign's whole premise is the memory arithmetic this audit just found to be 6.5× wrong.

---

## 13. What the fix pass rejected (2026-07-16)

Fixers were required to re-verify every finding before touching it. Six were rejected — this list is
as valuable as the fixes, because acting on any of them would have made a file *less* true.

1. **The TEXT-reassembly cluster (rejected before the pass even launched).** One auditor claimed
   multi-fragment TEXT is reassembled device-side and that `System_ESPNow.h:552` / `:773-784`
   ("per-fragment, never a reassembled lump") were false. **They are true.**
   `System_ESPNow.cpp:4713` excludes `ESPNOW_V4_TYPE_TEXT` from reassembly; the `payload = e->buffer`
   line cited as proof sits *inside* that exclusion. Acting on it would have rewritten honest
   comments into lies and left the real lie (`:2706`) standing.
2. **`gPeerIdentities` PSRAM/DRAM contradiction** — does not exist. Three agents independently
   confirmed the comment and the `EXT_RAM_BSS_ATTR` definition agree. (It was a stale example: the
   contradiction was real earlier the same day and had already been fixed.)
3. **`:14478` is not a comment** — it is a `snprintf` string literal. Editing it would have been a
   code change. The real comment at `:14897` was fixed instead; the string is listed in §14.
4–6. **Three real defects that need CODE, not comments** — the `V4RxCtx` ODR violation,
   `sessionAllocate`'s incomplete clear, and `espnowregenidentity`'s unfulfilled wipe. Each got an
   honest comment describing the current (bad) behavior; the repairs are in §14.

One fixer, while correcting `espnowmeshes remove`, wrote *"disabled slots keep their label until
overwritten by a future add"* — then checked `findFreeMeshSlot`, found it requires `!enabled` **and**
an empty label, realized it had just authored a **new** falsehood, and corrected it to record the
real trap: a removed slot is permanently spent until re-enabled under its old label. That is the
process working.

---

## 14. The code fixes

### ✅ Done 2026-07-16 (built green, verified in the ELF)

| # | Defect | What shipped |
|---|--------|--------------|
| 3 | `espnowbuffers` × 4 settings write-only, CLI claimed "takes effect after reinit" | **Deleted.** The command, its 4 generated setter commands (`espnowtxqueuesize`/`rxbuffersize`/`chunksize`/`filechunksize`), 4 forward decls, 5 registry rows, 4 schema rows, 4 macro lines and the 4 `gSettings` fields. Verified: 0 symbols, 0 strings in the ELF. |
| 4 | `espnowmeshes add <label> <pass>` silently dropped the passphrase | **Rejects a 2nd arg** with an error naming the real two-step flow. (Naming meshes always worked — only the docblock lied.) |
| 5 | `espnowregenidentity --confirm-wipe-all-bonds` deleted no peer records | **Honours the flag.** After a successful regenerate it calls `peerIdentityForget()` for every valid slot and reports `bonds wiped: N`. |
| 9 | `chunksTimedOut` printed as "Reassembly Timeouts", never incremented | **Deleted** — field, ctor init, JSON key `reassemblyTimeouts`, and the human line. Expiry is still reported by `v4FragRxGc` ("Reassembly GC"), which has a live bump site. |

**Why deleting `espnowbuffers` was right, not just easy.** Wiring it up is a *protocol* change, not a
small job: the receiver computes each fragment's offset from a **compile-time constant**
(`:4787 offset = h->fragIndex * V4_MAX_FRAGMENT_PAYLOAD`), and sizes its buffer the same way, so a
sender honouring a runtime `chunkSize` would write at one stride while every receiver reads at
another — garbage reassembly. Fragment size is a wire contract; honouring it means negotiating it
per-session. The advertised ranges were already impossible anyway: `chunkSize` accepted 100–**212**
against a 202-byte `MAX_PLAINTEXT`, and `fileChunkSize` accepted 100–**216** when the file payload
already seals to exactly 218 = the hard cap with zero headroom.

**Why the regen wipe mattered more than it looked.** `findFreeSlot()` returns the first `!valid`
slot of 16. Stale records stay `valid` forever, so regenerate-with-a-full-table would have **locked
out every future pairing** — a hard failure, not cosmetic.

### Still open — need a decision

| # | Defect | Fix shape | Risk if left |
|---|--------|-----------|--------------|
| 1 | `MemoryMonitor.cpp:84` admits ESP-NOW at 327,680 B PSRAM free; init needs ~855 KB | Raise the gate to the real figure | Guard passes, init fails |
| 2 | `V4RxCtx` re-declared in `Handlers_Crypto.cpp:43`, missing `isAuthenticated`/`isSessionEncrypted` (ODR) | Delete the copy; include the real definition | Latent: first read of `deviceName` silently returns an auth flag |
| 6 | `sessionAllocate` reuse path leaves `aeadKeyRxPrev` + `prevKeysValidUntilMs` + `rekeyEphPrivKey` + `bondToken` | `memset` like the free-slot branch | Stale key material outlives a re-handshake |
| 7 | Six user-facing usage strings print the dead two-word CLI form | One-word tokens | "Unknown command" |
| 8 | `:15497` log string still says "older logical messages dropped" (inverted — it drops newest) | Reword | Misleads at runtime |
| 10 | `growPeerHistoryArray` transient 2× peak (§2.2) | Array-of-pointers (LAZY_ALLOCATION_AUDIT Tier 1) | Growth fails on a loaded device |
