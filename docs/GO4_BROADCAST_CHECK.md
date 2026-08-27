# GO4 — Does the plaintext-multi-frame guard break broadcast?

**ANSWER: NO.** The guard is safe as written. No broadcast in this firmware can be
`fragCount > 1`, so the `!wasSessionEncrypted` term never fires on a broadcast. The
guard's two conditions are conjunctive, and the `fragCount > 1` condition is the one
that saves it — not the encryption condition.

Judge's note on the owner's premise: the premise ("a broadcast has no per-peer session,
so it plausibly cannot be `wasSessionEncrypted`") is **correct on the transmit side and
false as a receive-side invariant**. The RX path never looks at the destination MAC. That
asymmetry does not create a break — it only makes the guard *more* permissive than the
owner assumed — but it must not be written into a code comment as if RX enforced it.
Details in §2.

Scope: read-only review of the working tree at HEAD. Nothing was edited, built or flashed.

---

## 1. The change under review

Insert immediately before `System_ESPNow.cpp:5316`:

```c
if (h->fragCount > 1 && h->type != ESPNOW_V4_TYPE_ACK &&
    h->type != ESPNOW_V4_TYPE_TEXT && !wasSessionEncrypted) {
  return true;   // drop plaintext multi-frame
}
```

Existing line it precedes (`System_ESPNow.cpp:5316`):

```c
if (h->fragCount > 1 && h->type != ESPNOW_V4_TYPE_ACK && h->type != ESPNOW_V4_TYPE_TEXT) {
```

Placement is correct. `wasSessionEncrypted` is declared at `System_ESPNow.cpp:5169` and set
at `System_ESPNow.cpp:5288`, inside the `SESSION_FRAME` unwrap block that opens at
`System_ESPNow.cpp:5252` — strictly upstream of 5316. An AEAD fragment is therefore already
flagged when the guard evaluates.

---

## 2. Can a broadcast ever be session-encrypted?

Two different questions hide behind this one. They have different answers.

### 2a. Can any sender in this firmware emit a session-encrypted frame to FF:FF:FF:FF:FF:FF? **NO.**

Both AEAD emitters hard-require a peer identity *and* an ACTIVE session for the destination,
resolved before a single byte goes out:

`System_ESPNow.cpp:1733-1741` (`v4_send_session_wrapped`):
```c
const PeerIdentity* pid = peerIdentityFindByMac(dst);
if (!pid) { setErr("no peer identity — run espnowkeyex first"); return false; }
SessionState* s = sessionFindByPeer(dst, pid->meshId);
if (!s || s->state != SESSION_ACTIVE) { setErr("no ACTIVE session — run espnowsessionopen first"); return false; }
```

`System_ESPNow.cpp:2124-2135` (`v4_send_encrypted_chunked`) repeats the same two checks
verbatim. FF:FF:FF:FF:FF:FF has neither: it is registered only as a raw ESP-NOW peer with
`encrypt = false` at `System_ESPNow.cpp:9907`, never as a `PeerIdentity`. Peer identities are
written only by the KEY_EX handlers (`System_ESPNow_Handlers_Crypto.cpp:300` and `:404`).

So on the wire, from this firmware: broadcast ⟹ plaintext. The owner is right.

### 2b. Would the RX path *accept* a session-encrypted frame delivered to the broadcast address? **YES — nothing stops it.**

This is where one agent was right and three were wrong, and it matters for how the change
gets commented, not for whether it is safe.

- Session lookup is keyed on **source** MAC only:
  `System_ESPNow.cpp:5253` → `SessionState* s = sessionFindBySessionId(h->sessionId, recv_info->src_addr);`
  and `System_ESPNow_Sessions.cpp:50-61` matches on `sessionId` + `memcmp(s.peerMac, peerMac, 6)`.
- The AEAD AAD is header bytes 0..29 — `System_ESPNow_Sessions.cpp:317` (seal) and
  `:347` / `:354` (open), all `aad, 30`. The `EspNowV4Header` carries no destination field, and
  the radio destination is not bound.
- The destination is *captured* and *rebuilt* but never read. Exhaustive grep for `des_addr`
  across `components/hardwareone/` returns 5 hits: `System_ESPNow.cpp:383` (comment),
  `:1109` (write, in the RX callback), `:6108` (comment, TX callback), `:8495` (comment),
  `:8516` (write, in the ring drain). **Zero reads** inside `v4_try_handle_incoming`
  (`System_ESPNow.cpp:5124`–`5630`) or any `v4h_*` handler.

So "a broadcast is never session-encrypted" is a **TX-side accident**, not an RX-side rule.
A frame that arrived at the broadcast MAC carrying a valid `SESSION_FRAME` from a peer we
have a session with would decrypt and set `wasSessionEncrypted = true` at
`System_ESPNow.cpp:5288`.

**Why this does not endanger the guard:** it can only make a frame *pass* the guard that the
owner assumed would be dropped. It cannot cause a legitimate frame to be dropped. And such a
frame is not functionally a broadcast — the key is pairwise, so exactly one device on the air
could decrypt it.

**Consequence for the commit:** do not write "broadcasts can't be session-encrypted, so this
is safe." Write "no sender emits a multi-fragment broadcast" — that is the load-bearing fact
and it is enforced in code (§3).

---

## 3. Can any broadcast fragment today?

**No.** Only two of the four `esp_now_send` call sites in the entire first-party tree can
stamp `fragCount > 1`, and neither is reachable with a broadcast destination.

All four `esp_now_send` call sites (verified by grep over `components/` + `main/`; no
first-party use of the Arduino `ESP32_NOW` wrapper):

| # | Function | `esp_now_send` at | fragCount | Can target FF? |
|---|---|---|---|---|
| 1 | `v4_send_frame` | `System_ESPNow.cpp:1702` | **hardcoded 1** (`:1652`) | yes |
| 2 | `v4_send_session_wrapped` | `:1773` | **hardcoded 1** (`:1757`) | no (§2a) |
| 3 | `v4_send_encrypted_chunked` | `:2200` | computed (`:2176`) | no (§2a) |
| 4 | `v4_send_frag_ack` | `:2374` | echoes original (`:2368`) | no (unicast to fragment sender, `:5276`/`:5510`) |

`System_ESPNow.cpp:1652` is the chokepoint every broadcast passes through:
```c
h.ttl = ttl; h.fragIndex = 0; h.fragCount = 1;
```
and `System_ESPNow.cpp:1696`: `if (totalLen > 250) return false;`

That combination is decisive: **an over-long broadcast is refused, never split.** Payload
size is therefore irrelevant to this question.

### Per-sender table — everything that reaches FF:FF:FF:FF:FF:FF or fans out to all peers

Broadcast-MAC literals in the component (grep, `components/hardwareone/`): `:1108` (RX
compare), `:2418`, `:4797`, `:4827`, `:8510` (drain rebuild), `:9907` (peer registration).
Only three of those are sends.

| Sender | Type | Radio dst | Flags | Payload bound (proof) | fragCount |
|---|---|---|---|---|---|
| `sendPairBeacon` `:4793` | PAIR_BEACON (36) | **FF** `:4797` | BROADCAST_AUTH | 24 B fixed — `static_assert(sizeof(V4PayloadPairBeacon) == 24)` `Wire.h:257`; `deviceName` is `char[20]`, `strncpy`-truncated `:4796` | **1** |
| `sendPairCtrl` `:4822` | PAIR_REQUEST/ACCEPT/REJECT (40/41/42) | **FF** `:4827` | BROADCAST_AUTH | 28 B fixed — `static_assert(sizeof(V4PayloadPairCtrl) == 28)` `Wire.h:272` | **1** |
| `v4_broadcast_topo_request` `:2416` | TOPO_REQ (32) | **FF** `:2418` | **0 — no auth** | 8 B fixed (`V4PayloadTopoReq`, `Wire.h:282`) | **1** — and **DEAD CODE**, see below |
| `v4_broadcast` `:1916` | any | per-peer **unicast** `:1949` | ORs in BROADCAST_AUTH `:1942` | refused if `payloadLen + 32 > 218` `:1665-1670` | **1** |
| `v4_broadcast_category` `:1996` | any | per-peer **unicast** `:2049` | ORs in BROADCAST_AUTH `:2022` | same refusal | **1** |
| HEARTBEAT (30) via category | — | per-peer unicast | BROADCAST_AUTH | 32 B fixed — `static_assert` `Wire.h:242` | **1** |
| TIME_SYNC (35) via `v4_broadcast_time_sync` `:2391` | — | per-peer unicast | BROADCAST_AUTH | 16 B fixed (`V4PayloadTimeSync`, `Wire.h:275-279`) | **1** |
| SENSOR_STATUS (151) via category `:2604` | — | per-peer unicast | BROADCAST_AUTH | small fixed struct | **1** |
| `v4_broadcast_text` `:2712` (`espnowbroadcast`) | TEXT (52) | per-peer unicast | BROADCAST_AUTH | `textLen > 218` refused `:2713`; effective cap 186 after tag `:1665` | **1** |
| SENSOR_BROADCAST (150) | — | **TX deleted** `:2611-2614` | — | — | n/a |

**The naming trap.** `v4_broadcast` and `v4_broadcast_category` are **not** radio broadcasts.
Its own doc comment says so at `System_ESPNow.cpp:1912-1913`:

> `Sends to each active peer individually (ESP-NOW doesn't support true broadcast)`

They iterate `gMeshPeers` and unicast via `v4_send_frame` (`:1948-1949`, `:2048-2049`), merely
OR-ing `BROADCAST_AUTH` into the flags. So `espnowbroadcast` TEXT, HEARTBEAT, TIME_SYNC and
SENSOR_STATUS **never touch the FF address at all**. A future guard keyed on the
`BROADCAST_AUTH` flag as a proxy for "this was broadcast" would be wrong — that flag appears
overwhelmingly on unicast frames.

**`v4_broadcast_topo_request` is dead code.** Grep over `components/` + `main/` finds only the
forward declaration (`:292`), the definition (`:2416`), and its own internal call to
`v4_send_topo_request` (`:2419`). `v4_send_topo_request` itself (`:2401`) has no other caller.
The *live* TOPO_REQ path is `requestTopologyDiscovery` at `System_ESPNow.cpp:8318-8321`, which
sends **per-peer unicast** with `flags = 0`. Two agents mis-stated this as a live broadcast;
it is not. (This does not change any conclusion — it is `v4_send_frame`, so `fragCount = 1`
either way.)

### The one fan-out path that *can* fragment — and why it is fine

`meshBroadcastEnvelopeTyped` (`System_ESPNow.cpp:8271-8280`, used by `meshSendBootToPeers`,
type BOOT=31) fans out an unbounded JSON `String` via `v4_send_payload_smart` per peer. Above
`ESPNOW_V4_MAX_PLAINTEXT` (202, `Wire.h:40`) that reaches `v4_send_encrypted_chunked`
(`:2299`) and does emit `fragCount > 1` — but every fragment is `SESSION_FRAME`-sealed
(`System_ESPNow_Sessions.cpp:302` ORs the flag unconditionally), so RX sets
`wasSessionEncrypted = true` at `:5288` before the guard. It passes. It is a per-peer unicast
fan-out, not a radio broadcast. Flagging it only so a future reader does not mistake it for a
counterexample.

Closest call in the table, worth remembering if the payload ever grows: **SENSOR_ENVELOPE
(154)** is genuinely two fragments today. `v4_send_sensor_envelope` admits `jsonLen <= 200`
(`:2622`) and prepends a 4-byte packed header (`V4PayloadSensorBroadcast`, `Wire.h:348-353`),
so `totalLen` can reach 204 > 202 and take smart's multi-frame branch. Encrypted unicast, so
it passes the guard; and its dispatch row already carries `REQ_SESSION_ENC` (`:4982`).

---

## 4. Can a device broadcast a non-encrypted frame at all?

**Yes — every broadcast this firmware sends is non-encrypted.** Confidentiality never applies
to a broadcast here, by construction (§2a). What broadcasts get instead is *authentication*:

- `PAIR_BEACON` and `PAIR_REQUEST/ACCEPT/REJECT` go out to FF as **authenticated plaintext** —
  `ESPNOW_V4_FLAG_BROADCAST_AUTH` appends an HMAC-SHA256 over `header[0..29] || payload` keyed
  by the mesh group key (`System_ESPNow.cpp:1662-1687`). The comment at `:1621` says it
  plainly: *"Authentication, not confidentiality."* On RX, `wasAuthenticated` is set at `:5217`
  and `wasSessionEncrypted` deliberately is **not** — see the comment at `:5166-5168`.
- The dead `v4_broadcast_topo_request` path would send **fully unauthenticated** plaintext to
  FF (`flags = 0`, `:2411`), and its dispatch row has no gate: `System_ESPNow.cpp:4983`
  → `{ ESPNOW_V4_TYPE_TOPO_REQ, 0, v4h_topo_req }`.

One correction to a claim made in review: it is **not** true that a broadcast silently
downgrades to plaintext whenever no group key is cached. The strip only happens when a
fingerprint exists but the key does not (`System_ESPNow.cpp:1686-1691`). When
`meshFingerprint == 0` the outer `if` at `:1662` never runs, so the flag stays **set** with no
tag appended — and every receiver drops that frame at `System_ESPNow.cpp:5186-5189`
(`BROADCAST_AUTH requires mesh fingerprint`). Different failure mode, same net result: nothing
gets delivered.

Either way, all of these are `fragCount = 1`, so the guard cannot touch them.

---

## 5. Cross-mesh multi-fragment broadcast — what happens today

Three cases, and the second and third are real.

**(a) Foreign mesh with a different label → dropped before reassembly.** `System_ESPNow.cpp:5154-5157`:
```c
if (h->meshFingerprint != 0 && meshByFingerprint(h->meshFingerprint) == nullptr) {
  // Silent drop. Frame is for a mesh we're not a member of.
  return true;
}
```

**(b) `meshFingerprint == 0` → the gate is skipped by design.** The `!= 0` short-circuit is
deliberate ("no mesh scope", comment at `:5150-5153`), and `meshByFingerprint` also returns
`nullptr` for 0 (`:1308`). A frame with fingerprint 0, no `BROADCAST_AUTH` and no
`SESSION_FRAME` skips the block at `:5176`, skips the block at `:5252`, and lands squarely in
the reassembler at `:5316`.

**(c) The "gate" is a label check, not a membership check.** `meshFingerprintForLabel`
(`:1290-1293`) is CRC16-CCITT of the **label string only** — no key material. And every device
bootstraps `label = "primary"`, `enabled = true`, `isDefault = true` at boot even with no
passphrase (`initPrimaryMeshFromLegacySettings`, `:1486-1501`, called from `initEspNow` at
`:9573`). Two out-of-the-box fleets in RF range therefore share `CRC16("primary")` and each
accepts the other's frames past `:5154`. The field is plaintext and attacker-settable;
`Wire.h:248` says as much.

### Is it a DoS on the 2-slot table? **Yes, today. The guard closes it.**

- `V4_REASM_MAX 2` (`:144`), `V4_REASM_TIMEOUT_MS 5000` (`:145`), entries ~4.2 KB each in
  PSRAM (`:164`; `V4_FRAG_MAX` = 21 × 200 B, `:122`/`:112`).
- `v4_reasm_gc` runs **only** from inside the reassembly block (`:5329`) — i.e. only when
  another fragment arrives.
- Refreshing `e->lastUpdateMs` on any repeat fragment (`:5375`) extends the hold indefinitely
  at a few frames per second.
- With both slots held, every legitimate fragmented receive is dropped at `:5333-5337` — and
  the only fragmenting sender is the encrypted CMD_RESP / FS_LIST_REPLY / file path, so the
  practical symptom is bonded CLI output and the remote file browser going dead.

No eviction risk to in-flight legitimate messages: `v4_reasm_find_or_alloc` (`:189-209`)
matches on `(src, msgId)` and claims only `!active` slots — it never evicts an active entry.
So the attack is starvation, not corruption.

Cost to an attacker: two 32-byte frames per 5 s, no key material, no pairing. The guard
removes the primitive entirely, because any frame reaching `:5316` without an AEAD unwrap is
by definition unauthenticated.

### Bonus: the guard also closes a memory-safety bug on the plaintext path

`System_ESPNow.cpp:5348` bounds `h->fragIndex` against the **wire** `fragCount` (up to 255),
but `e->have[]` is sized `V4_FRAG_MAX` = 21 (`:158`, `:122`) and `e->fragCount` is clamped to
21 at allocation (`:203`).

```c
if (h->fragIndex >= h->fragCount) { ... return true; }   // :5348 — wire fragCount
if (e->have[h->fragIndex]) { ... }                        // :5354 — 21-element array
```

A frame with `fragCount = 22, fragIndex = 21` passes `:5348`, then reads `e->have[21]` — one
past the end. The corresponding **write** at `:5373` is normally caught by the offset guard at
`:5364`, except for exactly `payloadLen == 0`: `offset = 21 * 200 = 4200`,
`4200 + 0 > 4200` is false, so `e->have[21] = true` executes — a one-past-the-end write. A
32-byte header-only frame also skips the CRC check (`:5142-5143`, gated on `payloadLen > 0`), so it
is trivially craftable. By struct layout (`:157-159`, two byte arrays back to back) it lands
on `buffer[0]`, inside the same object — no fault, no off-object leak. **Confirmed as a
reachable OOB access; the "lands on buffer[0]" part is a layout inference, not
compiler-verified.**

The guard closes this for plaintext senders. It remains reachable by an *authenticated* peer
sending a malformed `fragCount`, so a separate one-line fix is still warranted: bound against
`e->fragCount`, not `h->fragCount`. Not part of this change.

---

## 6. The guard: keep it as written (with one optional tightening)

**The original is safe. Ship it as proposed if you want the minimum diff.** Broadcasts are
unaffected because §3 shows no broadcast sender can produce `fragCount > 1` — the drop
condition simply never evaluates true for one.

Clause-by-clause:

- `h->fragCount > 1` — the load-bearing clause. Every broadcast is 1 (`:1652`), so the guard
  is inert for broadcast regardless of what the encryption term does.
- `h->type != ESPNOW_V4_TYPE_ACK` — **mandatory, do not remove.** `v4_send_frag_ack` builds a
  bare 32-byte header with `h.flags = 0` (`:2362`) and copies the *original* message's
  `fragIndex`/`fragCount` (`:2367-2368`). Per-fragment ACKs are plaintext by design. Dropping
  them would starve every `V4FragAckWait` slot and make `v4_send_encrypted_chunked` time out on
  fragment 1 — breaking **all** multi-frame encrypted sends (CMD_RESP, FS_LIST_REPLY, file
  transfer). This mirrors the existing exemption at `:5316` and the long comment above it.
- `h->type != ESPNOW_V4_TYPE_TEXT` — **optional.** Multi-frame TEXT is deliberately not
  reassembled on-device (comment at `:5310-5315`); it is stitched client-side. Plaintext
  multi-frame TEXT is unsendable by any current build (`v4_broadcast_text` caps at one frame
  `:2713`; `espnowsend` goes through `sendAeadSync` → strict encrypt-or-fail, `:14615`), so
  removing this clause costs nothing and closes the one injection path the guard otherwise
  leaves open. Encrypted multi-frame TEXT is unaffected either way: it has
  `wasSessionEncrypted = true`, skips the drop, and still falls through to per-fragment
  dispatch because the exemption on the `if` at `:5316` stays.
- `!wasSessionEncrypted` — correct and correctly ordered (`:5288` precedes `:5316`). Note it is
  strictly narrower than `wasAuthenticated`: a `BROADCAST_AUTH` frame sets `wasAuthenticated`
  at `:5217` and leaves `wasSessionEncrypted` false (comment `:5166-5168`). That is fine —
  BROADCAST_AUTH frames with `fragCount > 1` are already refused upstream at `:5176-5180`.

Tightened version, if you want the extra hardening and a diagnosable field drop:

```c
// Only AEAD-authenticated fragments may enter the reassembler.
// The only multi-fragment sender in this firmware is v4_send_encrypted_chunked
// (line 2090): it requires an ACTIVE session and seals EVERY fragment as its own
// SESSION_FRAME. v4_send_frame — the primitive under every plaintext send AND
// every broadcast — hardcodes fragCount = 1 (line 1652) and refuses frames over
// 250 B (line 1696), so no broadcast is ever multi-fragment. (RX does NOT enforce
// that: it never inspects the destination MAC. It is a TX-side property.)
// BROADCAST_AUTH multi-fragment is already refused upstream at line 5176.
// ACK stays exempt: v4_send_frag_ack (line 2347) is plaintext by design and echoes
// the ORIGINAL message's fragIndex/fragCount; dropping it here would starve every
// V4FragAckWait slot and break all chunked TX.
if (h->fragCount > 1 && h->type != ESPNOW_V4_TYPE_ACK && !wasSessionEncrypted) {
  WARN_ESPNOWF("[V4_RX] plaintext multi-frame refused: type=%u frag=%u/%u msgId=%lu "
               "from %02X:%02X:%02X:%02X:%02X:%02X",
               h->type, h->fragIndex + 1, h->fragCount, (unsigned long)h->msgId,
               recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
               recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
  return true;
}
```

**Do not** restrict the drop to unicast-only, and do not special-case broadcast types. The
destination *is* available at the guard site (`recv_info->des_addr` is truthfully rebuilt at
`:8509-8516`), but using it would weaken the guard to accommodate a sender that cannot exist.

**Side effects of the guard, checked:**
- *ACK behavior:* unchanged. Plaintext multi-frame fragments never reached the ACK-send at
  `:5506-5516` today — incomplete fragments return at `:5404` from inside the reassembly block.
  Encrypted fragments are ACKed inside the unwrap block at `:5274-5280`, upstream of the guard.
- *Slot cleanup:* no obligation is skipped. The guard returns before
  `v4_reasm_find_or_alloc` (`:5332`), so no slot is ever allocated for a plaintext fragment,
  and none can be left half-filled. (This resolves the open question one agent flagged.)
- *Relay:* no multi-hop forwarding exists — only Phase 0 groundwork (`:382`, `:405`) and the
  `espnowrelayblock` RX-side diagnostic (`:8501-8502`). Nothing re-emits a received fragment.
- *What actually stops working:* plaintext multi-frame CMD_RESP (51), STREAM (90),
  USER_SYNC (56) and BOOT (31) from a pre-2026-05 peer. Everything else in that class
  (FILE_*, FS_*, BOND_*, SENSOR_*) already carries `REQ_SESSION_ENC` and is dropped ~40 lines
  later at `:5098` anyway — the guard just moves the drop earlier, before a 4.2 KB slot is
  burned. Per the project's standing "no backwards compatibility needed" decision, the
  old-peer case is not a regression.

---

## 7. Confidence, disagreements resolved, and what remains UNVERIFIED

**Confidence: HIGH** on the verdict. The proof is structural, not enumerative: there are only
four `esp_now_send` call sites in the first-party tree, and every broadcast funnels through one
that hardcodes `fragCount = 1` and *refuses* (never splits) an over-long payload. Payload sizes,
device-name lengths and sensor counts are therefore irrelevant to the question — I chased the
"long friendly name" hypothesis specifically and both broadcast structs pin `deviceName` to a
`strncpy`-truncated `char[20]` behind a `static_assert`.

**Where the four reports disagreed, and the ruling:**

1. *"Can a broadcast be `wasSessionEncrypted`?"* — three agents said the owner's premise was
   simply correct; one said RX would accept such a frame. **The RX agent is right** (§2b:
   source-only session lookup, 30-byte AAD with no destination, zero `des_addr` reads). Both
   framings reach the same verdict, but only one is safe to put in a comment.
2. *"Is `v4_broadcast_topo_request` live?"* — one agent said dead, one said the firmware
   actively broadcasts TOPO_REQ, one implied it was live. **Dead** (§3: no callers anywhere).
   The live TOPO_REQ path is per-peer unicast at `:8318-8321`.
3. *"Does a broadcast downgrade to plaintext with no group key?"* — asserted flatly by one
   agent. **Partly wrong** (§4): with `fingerprint == 0` the flag is not stripped and the
   receiver drops the frame at `:5186-5189`.
4. *"Guard placement / slot-cleanup obligation"* — left UNVERIFIED by one agent. **Resolved**
   (§6): the guard returns before any allocation, so there is nothing to clean up.
5. *"`have[]` out-of-bounds"* — flagged LIKELY by one agent. **Confirmed reachable** (§5); the
   struct-layout detail (`have[21]` aliasing `buffer[0]`) remains an inference.

**Still UNVERIFIED / out of scope:**

- I did not build, flash, or test on hardware. Everything here is source reading at HEAD.
- A non-HardwareOne / hostile transmitter's full behavior space is not characterized beyond
  the header-field cases above. The relevant point is directional and holds regardless: the
  guard can only *drop* frames that no legitimate sender produces.
- `peerIdentityPersist` is keyed on the payload-claimed `senderMac`, validated only against the
  attacker-writable header `origin` field (`System_ESPNow_Handlers_Crypto.cpp:148`, `:300`).
  I did **not** chase whether a passphrase-holding attacker could thereby install a peer
  identity for FF:FF:FF:FF:FF:FF and open a session against it. Even if so, no caller ever
  passes FF as a `v4_send_payload_smart` destination, and the effect would be to make the guard
  *more* permissive — so it cannot turn this change unsafe. Noted as a separate thread.
- I did not audit which downstream handlers wrongly treat "passed the `:5154` fingerprint gate"
  as "same mesh". Given §5c, some probably do.

**Pre-existing issues surfaced in passing (none caused by this change, none fixed here):**

1. `espnowbroadcast` help text is wrong. `System_ESPNow.cpp:15940` states *"single frame,
   <= 218 bytes"*; the real cap is **186** — `v4_broadcast_text` admits up to 218 (`:2713`) but
   `v4_send_frame` then refuses `payloadLen + 32 > 218` once the HMAC tag is appended
   (`:1665-1670`). Messages of 187..218 bytes fail silently on every peer.
2. Doc drift in the wire header: `System_ESPNow_Wire.h:244-246` documents the pairing beacon as
   *"Sent PLAINTEXT to the FF broadcast address (flags=0)"*, but the sender at `:4798` passes
   `ESPNOW_V4_FLAG_BROADCAST_AUTH`. Code is authoritative. Anyone reasoning about broadcast
   authentication from `Wire.h` alone will reach a wrong conclusion.
3. Unauthenticated topology disclosure: TOPO_REQ's dispatch row has no gate (`:4983`), and
   `v4h_topo_req` replies with the peer list to `src_addr` (`:3329`). Combined with the
   fingerprint-0 exemption, any device on the air can ask for the peer table. The live sender is
   unicast-only, but the *handler* answers anyone.
4. The reassembly slot key omits `type` (`:192-194`) while the streaming family deliberately
   reuses one `msgId` across STREAM / CMD_RESP / FILE_* (`:5521-5525`). Two different-type
   multi-fragment messages from one source under one msgId would share a slot.
