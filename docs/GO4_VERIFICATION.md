# GO-4 Verification — V4 fragment reassembly auth-verdict laundering

## 1. VERDICT

**The attack is REAL and constructible — and there is a simpler, race-free variant the assessment did not find.**

Single most important piece of evidence — the three lines that make it true, read together:

```
System_ESPNow.cpp:192-193   if (gV4Reasm[i].active && gV4Reasm[i].msgId == msgId &&
                                memcmp(gV4Reasm[i].src, src, 6) == 0) { return &gV4Reasm[i]; }
System_ESPNow.cpp:151-162   struct V4ReasmEntry { active; src[6]; msgId; type; fragCount;
                                received; have[]; buffer[]; bufferSize; lastUpdateMs; };   // no auth field
System_ESPNow.cpp:5596-5597 if (v4_dispatch_table_try(recv_info, h, payload, payloadLen,
                                isPaired, wasAuthenticated, wasSessionEncrypted, deviceName))
```

A reassembly slot is joined on `(src, msgId)` with no crypto predicate, the slot has nowhere to record
crypto provenance, and dispatch is handed the **stack locals of whichever frame happened to complete the
slot** (`bool wasAuthenticated = false;` :5165, `bool wasSessionEncrypted = false;` :5169). Bytes deposited
by an unauthenticated frame are therefore dispatched under an authenticated frame's verdict, past
`V4_OPC_FLAG_REQ_SESSION_ENC` at :5098.

I verified every step of this chain personally by reading `v4_try_handle_incoming` (:5124-5629) end to end,
not by grep.

**Where the assessment is wrong about impact:** its worst-case (remote command execution via `CMD` /
`@BOND` token retention, argued by Agent 1) is **REFUTED** — see §3.1. The realistic reachable targets are
`FS_LIST_REPLY` and `SENSOR_ENVELOPE`: authenticated-looking data injection, not code execution.

---

## 2. Claim-by-claim

Legend: **D** = agents disagreed; my own reading settles it.

| # | Claim | Verdict | Code that settles it |
|---|---|---|---|
| 1 | `v4_reasm_find_or_alloc` matches slots on `(src, msgId)` only | **TRUE** | `System_ESPNow.cpp:192-193` — predicate is `active && msgId == msgId && memcmp(src,src,6)==0`. No `type`, `fragCount`, `sessionId`, or auth term. |
| 2 | The slot stores `type` but never reads it | **TRUE** | Sole write `System_ESPNow.cpp:202 gV4Reasm[i].type = type;`. Full-file `grep -n gV4Reasm` returns 34 hits (124, 164, 176-207, 5045-5047, 5496-5498, 5618-5622, 9689-9693) — `.type` appears in exactly one of them, the write. Struct is file-local (:151), array is `static` (:164) → no other TU can read it. |
| 3 | `wasAuthenticated`/`wasSessionEncrypted` are per-frame stack locals with no home in `V4ReasmEntry` | **TRUE** | Locals at `:5165` / `:5169`, assigned only at `:5217` (BROADCAST_AUTH) and `:5287-5288` (AEAD unwrap). Struct at `:151-162` has ten fields, none of them auth/session/sessionId. |
| 4 | Dispatch inherits the verdict of whichever fragment COMPLETED the slot | **TRUE** | On completion only the pointer moves: `:5427-5428 payload = e->buffer; payloadLen = reassembledSize;`. Same invocation falls through to `:5596-5597` passing its own `:5165`/`:5169` locals. Those exact booleans are the gates at `:5084` and `:5098`, and are copied into `V4RxCtx ctx{...}` at `:5118`. |
| 5 | CRC16 is unkeyed | **TRUE** | `:1263-1270` — `v4_crc16_ccitt(const uint8_t* data, size_t len)`, init `0xFFFF`, poly `0x1021`, **no key parameter**. TX stamps it identically at `:1698`. |
| 6 | CRC16 is skipped for SESSION_FRAMEs | **TRUE but not load-bearing** | `:5141-5143 const bool isSessionFrame = (h->flags & ESPNOW_V4_FLAG_SESSION_FRAME) != 0; if (!isSessionFrame && payloadLen > 0 && v4_crc16_ccitt(...) != h->crc16)`. The attacker's frame is *not* a SESSION_FRAME, so its CRC **is** checked — it just doesn't matter, because #5 makes it computable. Also skipped for `payloadLen == 0`, which the race-free variant (§3.2) relies on. |
| 7 | Mesh fingerprint 0 is allowed | **TRUE** | `:5154 if (h->meshFingerprint != 0 && meshByFingerprint(...) == nullptr)` — zero short-circuits. Documented deliberate at `:5150-5153`. Contrast `:5186-5189`, where BROADCAST_AUTH *does* reject fingerprint 0 — the codebase knows how and chose not to here. |
| 8 | `isPaired` and the `REQ_*` flags are all evaluated AFTER reassembly | **TRUE** | Reassembly block `:5316-5434`. `isPaired` first computed `:5543-5551`. `REQ_PAIRED` `:5062`, `REQ_AUTHENTICATED` `:5084`, `REQ_SESSION_ENC` `:5098` — all inside `v4_dispatch_table_try`, whose only call site is `:5596`. A non-completing fragment returns at `:5404` and reaches none of them. |
| 9 | Peers added with `peerInfo.encrypt = false` | **TRUE (but documented intentional design)** | `:926 peerInfo.encrypt = false;  // LMK removed — confidentiality via SESSION_FRAME`, `:9909` (broadcast peer), `System_ESPNow_Handlers_Crypto.cpp:108`. No `esp_now_set_pmk` anywhere in `components/hardwareone`. **Declared deliberate at `:906-911`.** This is the exact shape of this project's prior error #1 — report it as an accepted architectural trade-off, not a misconfiguration. |
| 10 | "…so source MACs are spoofable" | **FALSE as causation** — **D** | Agents 1, 2, 4 flag the causation; Agent 3 accepted the phrasing. **I side with 1/2/4.** 802.11 source addresses were never authenticated by the ESP-NOW peer table; `encrypt=false` removes the *link-layer check that would have rejected* a plaintext frame bearing a peer's MAC. Net effect identical, reasoning inverted. **The real, unstated precondition:** the attacker needs raw 802.11 injection with a forged source MAC. A plain `esp_now_send` stamps the attacker's own MAC, which lands in a *different* slot (`:193`) that no genuine fragment will ever complete → `wasSessionEncrypted=false` → clean drop at `:5098`. |
| 11 | The V4 header carries `msgId`/`fragIndex`/`fragCount` in cleartext even for SESSION_FRAMEs | **TRUE** | `System_ESPNow_Wire.h:209-229` — packed 32-byte plaintext prefix; `msgId` at 8-11, `fragIndex` 19, `fragCount` 20, `type` 3. `esp_now_send(dst, frame, frameLen)` at `:1773` / `:2200` puts them on air verbatim. |
| 12 | "Only the payload is sealed" — header enjoys no cryptographic protection | **PARTLY-TRUE — Agent 3's correction is right** — **D** | Header bytes **0-29 ARE AAD-bound** to the Poly1305 tag: `System_ESPNow_Sessions.cpp:308-317` (`// AAD = first 30 bytes of header (everything except crc16)` … `aad, 30`) and the mirrored RX at `:345-347`. Only `crc16` (bytes 30-31) is outside, and it is zeroed at seal (`Sessions.cpp:303`) and skipped on RX. Consequence triad: **reading** `msgId` off the air = possible; **rewriting** a captured sealed fragment's header = impossible (AEAD open fails, `Sessions.cpp:362-366`); **originating** a fresh plaintext fragment with a chosen `msgId` = unaffected, because a frame without the SESSION_FRAME flag never enters the unwrap branch at `:5252` and so has no AAD check to fail. **The correction is real and does not rescue the design.** |
| 13 | ACK for a fragment is sent BEFORE the duplicate check | **PARTLY-TRUE — Agent 4's narrowing is right** — **D** | TRUE on the SESSION_FRAME path: `:5274-5280` (inside the `:5252` block) precedes the reassembler at `:5316` and the dup check at `:5354`; flag then cleared at `:5283`. FALSE on the plaintext path: that ACK block is at `:5507-5516`, *after* reassembly, and a non-completing plaintext fragment returns at `:5404` and is never ACKed. Since plaintext multi-frame TX was deleted (`:273-275`), the security-relevant instance is the one that holds. |
| 14 | Duplicate check is first-writer-wins (attacker must win a race per index) | **TRUE** | `:5354-5358 if (e->have[h->fragIndex]) { … return true; }`; store is past the gate at `:5372-5375`. |
| 15 | The race is practical, not theoretical | **TRUE** | Sender is strictly sequential and blocks per fragment: `:2170 h->flags = baseFlags \| ESPNOW_V4_FLAG_ACK_REQ;`, `:2147 const uint32_t ACK_TIMEOUT_MS = 200;`, `:2210-2218 while ((millis() - waitStart) < ACK_TIMEOUT_MS) { if (ackWait->acked) …; vTaskDelay(pdMS_TO_TICKS(10)); }`. Fragment 0 leaks `msgId` **and** `fragCount` in cleartext, so one sniffed frame gives every remaining index and ~10 ms+ of window each. Losing the race is invisible to the sender because of #13. |
| 16 | The escalating direction is injecting NON-final indices | **TRUE — assessment understates this** | If the attacker's plaintext frame completes the slot, that invocation carries `wasSessionEncrypted=false` → dropped at `:5098` (fail-closed). The working exploit seeds indices `1..N-1` and lets the sender's genuine sealed final fragment complete. |
| 17 | Impact reaches `CMD` → RCE under `kBondAdminUser` (Agent 1) | **FALSE — refuted** | See §3.1. `v4h_cmd` caps at `ctx.payloadLen <= ESPNOW_V4_MAX_PAYLOAD` (`:2902`; = 218, `Wire.h:30`), and the `reassembledSize` arithmetic at `:5408-5417` makes every attacker-favourable ordering exceed it. |
| 18 | Realistic reachable targets are the `REQ_SESSION_ENC` multi-fragment rows | **TRUE** | `{ FS_LIST_REPLY, REQ_PAIRED\|REQ_SESSION_ENC, v4h_fs_list_reply }` at `:5009` — a full reply is 13 fragments (`:135-137`) and the handler applies **no length cap**: `:3907-3909 fsListOnReplyReceived(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);`. `SENSOR_ENVELOPE` at `:4982` same gating. |
| 19 | The auth-verdict inheritance is documented as intentional somewhere | **FALSE** | Checked specifically, because this project's prior error #1 was exactly that miss. `:5220-5236`, `:5293-5315`, `:5426-5433` and the struct at `:151-162` are all silent on crypto provenance. The one nearby "this is deliberate" comment (`:273-275`) blesses *plaintext reassembly tolerance*, a different thing. |
| 20 | `ENABLE_BONDED_MODE` is actually on in this build | **TRUE** | Checked because prior error #2 was a triage citing an unchecked build flag. `System_BuildConfig.h:320 #define ENABLE_BONDED_MODE 1`; the only override is `:727-730 #if !ENABLE_ESPNOW → 0`, which cannot fire in an ESP-NOW discussion. Bond rows at `:5015-5024` are live. |

---

## 3. Corrections to the assessment

### 3.1 REFUTED: the "RCE via CMD" impact claim (Agent 1's headline)

Agent 1 argued the worst case is remote command execution — poison a later fragment, keep the victim's
genuine `@BOND` token from fragment 0, substitute the command text. **This does not work**, and the reason
is one of the adjacent bugs:

`v4h_cmd` refuses anything over one frame's worth:

```
System_ESPNow.cpp:2902   if (ctx.payloadLen > 0 && ctx.payloadLen <= ESPNOW_V4_MAX_PAYLOAD && gEspNow && !gEspNow->deferredCmdPending) {
System_ESPNow_Wire.h:30  #define ESPNOW_V4_MAX_PAYLOAD  (250 - 32)    // 218
```

and the completion-size arithmetic uses the **completing frame's** length for the last slot:

```
System_ESPNow.cpp:5408-5417
  for (uint8_t i = 0; i < e->fragCount; i++) {
    if (i == e->fragCount - 1) { reassembledSize += payloadLen; }   // completing frame's length
    else                       { reassembledSize += V4_MAX_FRAGMENT_PAYLOAD; }   // 200
  }
```

Enumerate the orderings for a 2-fragment CMD (the only fragment count that can fit under 218 at all):

- Attacker poisons index 1, genuine index 0 completes → `reassembledSize = 200 + 200 = 400 > 218` → **dropped at :2902**.
- Attacker's index-1 frame completes → `wasSessionEncrypted = false` → **rejected at :5740** (`v4_handle_cmd`'s own gate).
- Attacker poisons index 0 → he owns the credential region and has neither the bond token (sealed inside the AEAD) nor a password → **auth fails**.

3+ fragments are worse: `reassembledSize >= 400` always. **CMD is not exploitable through the reassembler.**
The buggy arithmetic accidentally defends it.

*(Functional side-effect worth a separate ticket: this means a legitimately fragmented CMD — any remote
command payload over 202 bytes — is silently dropped and has never worked. Dead path, not a vulnerability.)*

### 3.2 UNDERSTATED: there is a race-free, single-frame variant

The assessment (and every agent) framed this as a race the attacker must win per index. **It isn't
necessarily.** The bounds check compares `fragIndex` against the **wire** `fragCount`, while the buffer and
`have[]` semantics come from the **clamped, first-writer** `e->fragCount`:

```
System_ESPNow.cpp:5348   if (h->fragIndex >= h->fragCount) { … return true; }        // WIRE fragCount
System_ESPNow.cpp:5361   uint16_t offset = h->fragIndex * V4_MAX_FRAGMENT_PAYLOAD;
System_ESPNow.cpp:5364   if (offset + payloadLen > e->bufferSize) { v4_reasm_reset(*e); … return true; }
System_ESPNow.cpp:5372-5375  memcpy(e->buffer + offset, payload, payloadLen);
                             e->have[h->fragIndex] = true; e->received++; e->lastUpdateMs = millis();
```

Take an in-flight 13-fragment `FS_LIST_REPLY` (`e->fragCount = 13`, `bufferSize = 2600`). Send **one**
32-byte plaintext frame: spoofed src, sniffed `msgId`, `type = FS_LIST_REPLY`, `fragCount = 14`,
`fragIndex = 13`, `meshFingerprint = 0`, `payloadLen = 0`. Trace it:

- `:5142` — CRC skipped (`payloadLen > 0` is false). No key needed.
- `:5154` — fingerprint 0 passes.
- `:5176` — no BROADCAST_AUTH flag → HMAC never runs.
- `:5252` — no SESSION_FRAME flag → **no crypto is consulted at all**.
- `:5316` — `fragCount > 1`, type is neither ACK nor TEXT → enters reassembler.
- `:5332` — joins the victim's slot on `(src, msgId)`.
- `:5348` — `13 >= 14`? No. Passes.
- `:5354` — `e->have[13]` is false. Passes.
- `:5364` — `2600 + 0 > 2600`? No. Passes.
- `:5373-5374` — `have[13] = true`, `received++`. **A phantom fragment.**
- `:5404` — returns "not complete yet". Silent: no ACK is emitted on this path.

The message now completes **one genuine fragment early**, on a genuine AEAD frame, so
`wasSessionEncrypted = true` and it sails past `:5098`. The 200-byte hole is whatever was in the slot
before — `v4_reasm_reset` (`:166-173`) clears `active/msgId/received/fragCount/src/have` and **never clears
`buffer`**.

**No race. One frame. No key.** Content control requires additionally grooming the slot's residue
beforehand (two slots total, `V4_REASM_MAX 2` at `:144`, first-free allocation at `:197-198`, buffer never
zeroed on reset); the code facts enabling that are verified, the orchestration is analysis, not test.

This makes the finding *worse* than described, and it also means any fix that only compares auth verdicts
per fragment must still bound `fragIndex` against `e->fragCount`.

### 3.3 Wrong reasoning, right conclusion (three places)

1. **"peers added with `encrypt=false` **so** source MACs are spoofable"** — causation inverted (row 10).
   Worse, `encrypt=false` is declared-intentional at `:906-911`; presenting it as a contributing defect
   repeats this project's prior error #1. The attack does not depend on it being a mistake; it depends on
   there being no radio-layer authentication *by design*, which shifts all the weight onto whether the
   application-layer AEAD covers every dispatch path — and the reassembler is precisely where it doesn't.

2. **"CRC16 unkeyed **and** skipped for SESSION_FRAMEs"** — true individually, misleading paired. The
   attacker's frame is not a SESSION_FRAME, so its CRC *is* checked (`:5142`); it is satisfiable, not
   absent. The skip that actually matters is the `payloadLen > 0` conjunct (§3.2).

3. **"…header carries msgId in cleartext, so an attacker can sniff and inject"** — the *sniff* half is
   right, but the assessment implies the header is cryptographically naked. Bytes 0-29 are AAD-bound
   (`Sessions.cpp:317`, `:347`). Any remediation leaning on "the header is already authenticated" is wrong
   for the same reason the attack works: unsealed frames never reach the AAD check.

### 3.4 Missing from the assessment: the direction constraint

Injecting the **completing** fragment fails closed at `:5098` (and `:5740` for CMD). The exploit must seed
non-final indices. State this or the finding reads as more deterministic than it is.

---

## 4. The adjacent bugs — separate verdicts

These had far less scrutiny than the main finding. Each stands on its own.

### 4.1 `fragIndex` bounds-checked against the wire `fragCount` — **TRUE, severity mis-stated**

`:5348` compares two header fields (`h->fragIndex >= h->fragCount`), never `e->fragCount` or `V4_FRAG_MAX`.
`have[]` is 21 elements (`:158`, `V4_FRAG_MAX` = 21 from `:122` + `CMD_RESULT_MAX 4096`).

- **Out-of-bounds READ** at `:5354` for `fragIndex` in 21..254 — **TRUE**. Struct layout puts `have` at
  offset 15 and `buffer` at 36 (`active`@0, `src`@1-6, `msgId`@8 after 4-align, `type`@12, `fragCount`@13,
  `received`@14, `have`@15-35, `buffer`@36), so `have[21] == buffer[0]`. Every such access stays inside the
  same 4244-byte entry: **cannot fault, cannot touch another allocation.**
- **Out-of-bounds WRITE** at `:5373` — the assessment's `have[254]` framing is **WRONG**. `offset` is
  `uint16_t` and maxes at 254×200 = 50800 (no wrap), and `:5364` rejects everything where
  `offset + payloadLen > bufferSize`. Agent 5 correctly identified the single surviving case:
  `fragIndex == 21`, `payloadLen == 0`, `e->fragCount == 21` → writes one byte of `1` into the entry's own
  `buffer[0]`. **Not heap corruption.**
- **The generalization neither the assessment nor Agent 5 stated:** the same `payloadLen == 0` construction
  works at `fragIndex == e->fragCount` for **any** `e->fragCount`, staying entirely in-bounds. That is the
  phantom-fragment primitive of §3.2, and it is the one that matters.

### 4.2 `reassembledSize` from the completing frame's `payloadLen` — **TRUE**

`:5408-5417`, with the source admitting it: `// We need to track this - for now estimate conservatively`.
Per-fragment lengths are never stored. Max over-report is 20×200 + 218 = 4218 against a 4200-byte
`buffer[]` (`:159`); for `gV4Reasm[1]` that also runs 10 bytes past the pool allocated at `:9690-9691`.

Reachability qualifier the assessment omitted: a conforming peer can never trigger it —
`v4_send_encrypted_chunked` is strictly in-order (`:2150`) with a blocking per-fragment ACK. It needs a
hostile sender, which §3.2 supplies. **UNVERIFIED whether any handler consumes the full mis-sized length:**
`v4h_cmd_resp` clamps (`:2928 if (resultLen > V4_REASM_MAX_MSG) resultLen = V4_REASM_MAX_MSG;` against a
`V4_REASM_MAX_MSG + 1` alloc), yielding at most a 1-byte overread; `v4h_fs_list_reply` (`:3907-3909`) passes
`ctx.payloadLen` straight through with no cap and was not traced further.

### 4.3 Unauthenticated remote teardown of any in-flight reassembly — **TRUE, and not in the assessment**

Agent 6's find, verified. A bare 32-byte plaintext frame with `type = ACK` and `fragCount > 1`:

```
System_ESPNow.cpp:5494-5502
  if (h->fragCount > 1) {
    for (int i = 0; i < V4_REASM_MAX; i++) {
      if (gV4Reasm[i].active && gV4Reasm[i].msgId == h->msgId &&
          memcmp(gV4Reasm[i].src, recv_info->src_addr, 6) == 0) { v4_reasm_reset(gV4Reasm[i]); break; }
```

`payloadLen == 0` skips CRC, fingerprint 0 passes, `type == ACK` skips the reassembler at `:5316`. **No
crypto gate on this path at all.** One frame kills any in-flight transfer. Note this is the *same DoS shape*
as the previously-rejected "drop the slot on mismatch" fix — it already exists in shipped code.

### 4.4 Fragment-ACK waiter matched without checking the source MAC — **TRUE, not in the assessment**

```
System_ESPNow.cpp:5469-5471
  if (gV4FragAckWait[i].active && gV4FragAckWait[i].msgId == h->msgId &&
      gV4FragAckWait[i].fragIndex == h->fragIndex) { gV4FragAckWait[i].acked = true;
```

`dstMac` is stored (`:216`, written at `:236`) and **never compared**. Any radio-range attacker can forge a
plaintext frag-ACK for a sniffed `(msgId, fragIndex)` and make *our* chunked sender advance past a fragment
the peer never received — silently incomplete transfer on the far side, reported as success locally.
Comparable severity to GO-4 and on the same transfer path. One-line fix.

### 4.5 Reassembly-slot exhaustion DoS — **TRUE**

`V4_REASM_MAX 2` (`:144`), `V4_REASM_TIMEOUT_MS 5000` (`:145`), and `v4_reasm_find_or_alloc` never evicts
(`:197-208`, returns `nullptr` when both are busy → `:5333` `"No reassembly slot available"`). Two forged
frames with distinct `msgId`s block every legitimate fragmented transfer for 5 s. No key, no pairing, no
race.

### 4.6 `gV4Reasm` null-deref — **TRUE, precondition-gated**

`v4_reasm_gc` (`:176`) and `v4_reasm_find_or_alloc` (`:190`) null-guard. Three sites do not: `:5045`
(`v4_dispatch_post_cleanup`), `:5496` (ACK branch), `:5618` (tail cleanup). `gV4Reasm` is nullable by design
— `:9691-9696` `ps_alloc(...)` with an explicit `"fragmentation disabled"` warning on failure. A frame with
`fragCount > 1` and `type == ACK` bypasses the reassembly block's own guard and reaches `:5496`. **Reachable
only on a device where the PSRAM reassembly alloc failed; not confirmed triggerable.**

---

## 5. The fix

### 5.1 Lead with the flaw: the proposed struct+lattice design is over-built

Agent 6's kill shot, verified: **the trust tuple can only ever take two values inside the reassembler**, so
the whole upgrade/downgrade/equal-trust-reset lattice collapses to one bit.

`wasAuthenticated` is set in exactly two places — `:5217` (BROADCAST_AUTH) and `:5287` (SESSION_FRAME
unwrap) — and BROADCAST_AUTH **cannot be fragmented**:

```
System_ESPNow.cpp:5176-5180
  if (h->flags & ESPNOW_V4_FLAG_BROADCAST_AUTH) {
    if (h->fragCount > 1) { DEBUGF(… "BROADCAST_AUTH cannot be fragmented — dropping"); return true; }
```

So every frame reaching `:5316` is either `(auth=F, enc=F)` or `(auth=T, enc=T)`. `authenticated` and
`sessionEncrypted` would be the same bit. The lattice is machinery for a case that cannot occur.

### 5.2 Preferred shape

One predicate at the reassembler gate:

```
:5316   if (h->fragCount > 1 && h->type != ACK && h->type != TEXT && wasSessionEncrypted) { … }
```

Verified safe and strictly stronger:

- **No legitimate sender breaks.** Exactly one TX site emits `fragCount > 1` for a reassembled type:
  `v4_send_encrypted_chunked`, `:2176 h->fragCount = fragCount;`, and every fragment is sealed at `:2180`.
  The only other `fragCount > 1` writer is `v4_send_frag_ack` (`:2368`) with `type = ACK` (`:2361`),
  excluded at `:5316`. All other senders hard-code `fragCount = 1` (`:1652`, `:1757`). Confirmed by
  enumerating every `fragCount =` assignment in the component.
- **Plaintext multi-frame TX no longer exists:** `:273-275 "(v4_send_chunked removed 2026-05 — plaintext
  multi-frame is unreachable now that smart is strict encrypt-or-fail. RX-side plaintext reassembly remains
  for one release window to tolerate old-firmware peers…)"`. Given this project's standing "no
  backwards-compat ever needed" policy (user erases before flashing), that release window has expired.
- **It also closes §3.2** — the phantom-fragment frame is plaintext, so it never enters the block — plus the
  OOB read at `:5354` and the attacker-triggerable reset at `:5364-5367`, none of which the struct fix
  touches.

### 5.3 If the struct fix is chosen anyway — conditions, all currently unstated

1. **ORDERING is load-bearing.** The check must run immediately after `v4_reasm_find_or_alloc` returns a
   matched slot, i.e. **before `:5348`**. Everything from `:5348` to `:5375` acts on the slot today with no
   auth notion — including the OOB read at `:5354` and `v4_reasm_reset(*e)` at `:5367`. Place the check
   after them and the attacker keeps both primitives against an encrypted slot.
2. **Do not refresh `e->lastUpdateMs` on a rejected fragment** (`:5375`) or an attacker pins the slot open
   past the 5 s GC indefinitely.
3. **Do not signal reject by returning `nullptr` from `find_or_alloc`** — the caller's only `nullptr`
   handler logs the wrong cause (`:5334 "all %u slots in use"`) and invites a future "fix" that allocates a
   fresh slot, reintroducing the bug.
4. **"Upgrade → reset" must mean a full `v4_reasm_reset`**, never a flags-only promotion — promoting in
   place would adopt already-stored plaintext and is strictly worse than doing nothing.
5. **Do NOT reset the slot on a verdict mismatch.** Repeat of the previously reviewed-and-rejected DoS. Note
   §4.3: that DoS *already exists* via the ACK path, so a fix that adds a second one while claiming to close
   a hole is indefensible.

### 5.4 Neither fix is sufficient alone

Whichever is chosen, it does **not** close §4.3 (unauthenticated ACK-path slot reset, `:5494-5502`), §4.4
(frag-ACK waiter with no source check, `:5469-5471`), §4.5 (2-slot exhaustion), or §4.2 (`reassembledSize`
arithmetic). Ship them together or the fix will be reported as closing a hole it half closes.

Also out of reach of any of it: TEXT is deliberately excluded from device-side reassembly (`:5316`) and
stitched client-side from per-piece tags (`:2877-2880`). **The same verdict-inheritance question exists one
layer up, on the client, and was not examined.**

---

## 6. What remains UNVERIFIED

| Item | Why it matters | What would settle it |
|---|---|---|
| **Whether an attacker can transmit an ESP-NOW frame with a forged source MAC on this hardware/IDF** | The entire chain depends on it — the slot is keyed on `src` (`:193`), so a non-spoofed frame lands in a slot no genuine fragment will complete. Commodity in principle; not verified in this repo. | Second ESP32 doing `esp_wifi_80211_tx` with a forged addr2, plus a packet capture at the victim. |
| **Whether ESP-IDF 5.5.1 delivers ESP-NOW frames from MACs absent from the peer table** | Not load-bearing (the attack spoofs an already-registered peer) but it bounds the pre-pairing variants. | Bench test + `espnowcapture` log. |
| **Whether `fsListOnReplyReceived` consumes the full mis-sized `payloadLen` from §4.2** | Decides whether §4.2 is stale-data or an overread past `gV4Reasm[1]`. | Read `System_ESPNow_FsList.cpp`; not traced in this pass. |
| **Whether the slot-residue grooming in §3.2 gives attacker-*chosen* content** | Separates "authenticated garbage" from "authenticated attacker payload". The code facts (2 slots, first-free alloc, `buffer` never cleared on reset) are verified; the orchestration is analysis. | Bench reproduction against a live FS_LIST transfer. |
| **§4.6 null-deref triggerability** | Requires a device whose PSRAM reassembly alloc failed. | Force `ps_alloc` failure and send a plaintext ACK with `fragCount > 1`. |
| **All timing figures** | Read from source (`ACK_TIMEOUT_MS 200`, 10 ms poll), never measured. | Logic analyser or capture. |
| **Every proposed fix** | Nothing here was compiled, flashed, or tested. Prior error #3 in this project was a *proposed fix* that was itself a bug. | Build + bench regression on a fragmented FS_LIST_REPLY. |

---

## 7. Confidence

**HIGH on the mechanism. MEDIUM on exploitability. HIGH on the impact ceiling being lower than claimed.**

- **Mechanism — HIGH.** Every structural claim was read directly in `v4_try_handle_incoming` (`:5124-5629`)
  and `v4_reasm_find_or_alloc` (`:189-210`), not inferred. Six agents independently reached the same reading
  and I re-derived it from source. I specifically hunted for the three prior failure modes: no comment
  anywhere blesses the verdict inheritance (row 19); the one build flag in play was checked, not assumed
  (row 20); and the fix section leads with the flaw, not the endorsement. The tracked source files are clean
  in git, so line numbers and the build object agree.

- **Exploitability — MEDIUM**, for exactly one reason: **forged-source-MAC 802.11 injection is an
  unverified precondition** for every variant. It is commodity capability and I have no reason to doubt it,
  but it was not demonstrated here and it is not a property of this codebase. Everything downstream of it is
  verified.

- **Impact — the assessment's ceiling is wrong.** "RCE under `kBondAdminUser`" is refuted by `:2902` +
  `:5408-5417` (§3.1). What actually lands is authenticated-looking **data injection** into
  `FS_LIST_REPLY` / `SENSOR_ENVELOPE` — a poisoned file listing or sensor reading presented to the master as
  AEAD-verified. Serious; not code execution.

**Action guidance:** the defect is real enough to fix on code-reading alone — it is a missing invariant, and
the one-line predicate in §5.2 is verified not to break any live sender. Fix it. But **do not publish a
severity rating or a CVE-style writeup** until the forged-MAC precondition is demonstrated on hardware and
the §3.2 residue-grooming question is answered, because those two together are the difference between
"authenticated garbage delivered" and "attacker-chosen authenticated payload delivered", and the assessment
currently asserts the latter without evidence.
