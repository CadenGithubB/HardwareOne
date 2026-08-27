# ESP-NOW Multi-Hop Relay Restore Plan

**Status: Phase 0 IMPLEMENTED (uncommitted, built green feathers3, HW test pending);
Phases 1+ not started. 2026-07-27.**

Phase 0 implementation notes (beyond §2's list, from the post-implementation review):
`linkRssiEwma` is an int16 in QUARTER-dB fixed point with a signum step (a whole-dB
int8 EWMA has a ±3 dB truncation deadband — exactly the planned route metric's
hysteresis margin); the RX callback's no-rx_ctrl fallback stores -128 (the "no sample"
sentinel), not -127; `espnowrelayblock` disarms before rewriting the MAC on re-arm;
`espnowmeshstatus`'s serialize cap was raised 1024 → GLOBAL_DEBUG_BUFFER_SIZE (4096)
since the new per-peer `linkRssi` key pressured the old cap; `espnowresetstats` now
also clears `gEspNowRxDrops`.

Produced by a 9-agent design study (5 constraint mappers → 3 competing designs → adversarial
red-team). This document is the surviving composite: every code claim below was verified
against the working tree; every number was re-checked. Line numbers reference the current
(uncommitted) tree and will drift.

Goal: make the mesh actually forward frames — today it is a single-hop star. `ttl` is
write-only, `espnowmeshttl`/`meshAdaptiveTTL` feed nothing, and the `espnowmeshmetrics`
forward/path/drop counters were removed in the 2026-06 audit because they had zero
increment sites.

---

## 1. The four constraints that shape everything

**C1 — ttl is inside both integrity envelopes.**
Header layout (System_ESPNow_Wire.h:209-229, 32 B packed): ttl sits at offset 18, and both
integrity schemes cover header bytes 0..29 (everything except crc16):
- BROADCAST_AUTH: HMAC-SHA256 keyed by the **symmetric per-mesh groupKey** every member
  holds (MeshKeys, Blake2b subkey "esp-grup"). A relay that verified the tag can therefore
  legally decrement ttl and **re-tag** (then recompute crc16 over payload+tag). Tamper
  protection is not weakened; ttl never leaves the MACed region.
- SESSION_FRAME AEAD: AAD = header bytes 0..29 under **pairwise** ChaCha20-Poly1305 keys
  (Sessions.cpp:162-197, 299-347). A relay cannot mutate ttl *or re-seal* — the keys are
  per-pair X25519. Encrypted traffic can never be forward-and-decremented in place.

**C2 — the header has an origin but no destination.**
`origin[6]` @12-17 exists and gV4Dedup already keys on it (relay-correct!), but there is no
final-destination field anywhere — destination is purely the `esp_now_send(dst,...)`
argument, which changes per hop. Routed unicast therefore needs a dst carried on the wire
(the envelope in §4 provides it without touching the 32-B header).

**C3 — RX keys several structures on the radio sender MAC, not origin.**
Session lookup is (sessionId, recv_info->src_addr) — System_ESPNow.cpp:5241-5251 — so a
forwarded SESSION_FRAME is dropped ("no active session"). Same radio-src keying: isPaired
gate, gV4Reasm reassembly, frag-ACK wait, deferred handler src captures
(deferredCmdSrcMac :2898, deferredCmdRespSrcMac :2922, deferredMetadataSrcMac), TEXT
attribution (:2855), SENSOR_STATUS attribution (:3075). Also: `onEspNowDataReceived`
discards `recv_info->des_addr` at ring-enqueue, so RX can't currently tell "unicast to me"
from broadcast. And every existing TX builder **re-stamps origin with the local MAC**
(v4_send_frame :1625, session_wrapped :1729, chunked :2148) — a forwarder must send raw
buffers, never reuse those builders.

**C4 — not all TX funnels through one place, and dedup has a bypass list.**
`v4_send_encrypted_chunked` (:2140-2176) and `v4_send_frag_ack` (:2330-2356) hand-roll
frames and call `esp_now_send` directly — any "stamp it in v4_send_frame" plan silently
misses them (this killed Design B's flag day and Design C's S3 choke point as specced).
gV4Dedup is bypassed for STREAM, BOND_STREAM_CTRL, CMD_RESP, FILE_* and msgId==0
(:5506-5526) — those types have **no loop suppression** and must never flood.

---

## 2. Recommended composite (red-team verdict)

Build **Design A (minimal flood) Stages 1-3 nearly verbatim**, fold in **Design B's
Stage 0 groundwork** as a companion, and keep **Design C's S2/S3 architecture on the
shelf** for the day routed unicast is wanted. Do NOT do a V5 header flag day: it shrinks
every payload fleet-wide, moves ttl outside the MAC (strict security downgrade), and buys
nothing the envelope approach doesn't.

What the near-term build delivers: broadcast **texts and time sync reach nodes one relay
hop (or more) past radio range**, `espnowmeshttl` and `espnowmeshmetrics` become real,
`meshAdaptiveTTL` (knob theater — no algorithm ever existed) is removed. Unicast
(chat/remote-cmd/files to out-of-range peers) stays single-hop until Phase R (§4).

### Phase 0 — Groundwork (Effort S, from Design B)
All relay-independent, verified-real fixes:
- Delete the duplicate `V4RxCtx` in System_ESPNow_Handlers_Crypto.cpp (ODR trap) — move
  the canonical struct to a shared header.
- Capture `recv_info->des_addr` (1 flag byte, `radioDstIsBcast`) into InboundRxItem.
- Thread the discarded `InboundRxItem.rssi` into `noteMeshPeerRxActivity` → per-peer
  `linkRssiEwma` (this is the true link RSSI; the heartbeat's rssi field is AP-RSSI).
- Surface the already-bumped-but-never-read `gEspNowRxDrops` in `espnowstats`.
- Fix `ensureUnencryptedPeer` frozen `info.channel = gEspNow->channel` → 0
  (follow-the-radio convention, Handlers_Crypto.cpp:130).
- New debug-gated test hook `espnowrelayblock <mac|clear>`: drop RX frames by radio src at
  the top of the drain — emulates out-of-range on a desk, needed for every later HW test.
- HW test: full 2-board regression smoke; asymmetric blocklist proves the hook (A marks B
  offline in 30 s while B still sees A).

### Phase 1 — Make the knobs honest (Effort S, Design A Stage 1)
- `v4_broadcast_text` (:2692) and `v4_broadcast_time_sync` (:2372): ttl literal →
  `gSettings.meshTTL` (live read, default 3). All ttl=1 sites stay 1 — which now *means*
  "never forwarded".
- Remove `meshAdaptiveTTL` completely (field, registry row, command, "adaptive" branch in
  cmd_espnow_meshttl, display lines). Settings loader ignores the stale JSON key.
- Own-origin guard: drop + count any frame with `isSelfMac(h->origin)`.
- RouterMetrics stage-1 fields with live bumps: `relayOwnOriginDrops`, `relayRxViaRelay`
  (origin != radio src on accepted BCA frames — the "multi-hop actually happened" gauge),
  plus display of rxRingDrops.
- HW test (2 boards): ttl visible in `espnowcapture` (**use `espnowbroadcast`** — the
  red-team caught that "espnowsendall" doesn't exist); heartbeats still ttl=1; adaptive
  rejected; full send/receive + timesync + file-transfer regression.

### Phase 2 — The forward engine (Effort M, Design A Stage 2 + red-team hardening)
New `v4_relay_maybe_forward()`, hooked in `v4_try_handle_incoming` immediately **after**
the dedup block (~:5528) — forward only verified, non-duplicate frames; never amplify
forgeries or dups.

Positive eligibility whitelist (**all** must hold):
`BROADCAST_AUTH verified` ∧ `type ∈ {TEXT(52), TIME_SYNC(35)}` ∧ `fingerprint != 0 with
known key` ∧ `fragCount == 1` ∧ `msgId != 0` ∧ `!isSelfMac(origin)` ∧ `ttl >= 2` ∧
`meshRelayEnabled` (new bool setting, default OFF) ∧ rate-limiter budget.

Never-relay (positive list means these can't regress): HEARTBEAT (hop-local by
definition), all SESSION_FRAME traffic (C1/C3 make it impossible and it would expose
@BOND tokens / user:pass under decrypt-relay), PAIR_* (pairing is proximity-scoped consent
by doctrine), TOPO_* (plaintext replies to radio src → orphaned reply storms), the dedup
bypass list, **SENSOR_STATUS** (handler attributes by radio src — relayed data would be
cached as the relay's; excluded until that's origin-keyed), BOOT (rides session-encrypted
unicast, can never hit the BCA gate anyway).

Mechanics:
- Build the forward in a 250-B PSRAM static (single-task guarantee, same pattern as
  plainBuf), copy header, ttl−1, strip ACK_REQ defensively, preserve origin/msgId/
  fingerprint verbatim, re-tag with groupKey (one tag serves all fan-out targets),
  recompute crc16, then raw `esp_now_send` per active gMeshPeers slot — skipping self,
  the arrival radio src, the origin, and peers on a different meshFingerprint.
- Inline on espnow_task (precedent: heartbeat fan-out already does N inline HMAC sends
  there); `JOB_RAW` is a trap (runJob rebuilds the header and re-stamps origin). A
  `JOB_FWD` clerk job is the documented escape hatch if soak shows RX-ring pressure.
- Rate limiter: 16 forwards / 10 s sliding window, compile-time (≈2.4% worst-case airtime
  duty). Not a setting.
- TEXT attribution fix: `slot.srcMac = ctx.h->origin` (not radio src) with name resolve
  against devices[] and MAC-string fallback. `noteMeshPeerRxActivity` stays on radio src —
  liveness is link-local.
- **Red-team required additions:**
  - History-slot guard: a groupKey holder can mint arbitrary origins and exhaust
    `findOrCreatePeerHistory` slots (:16266). Cap slot creation for origins not in
    devices[] (single shared "unknown senders" slot or unknown-only LRU) and count rejects.
  - TIME_SYNC freshness check moves here (not optional/later): v4h_time_sync applies any
    verified offset unconditionally (:2814-2830); relay widens the >5 s replay window from
    1 hop to TTL hops. Bound backward jumps / rate-limit applies, landing with the engine.
- Stage-2 metrics: relayForwards, relayFanoutUnicasts/Fails, relayDropTtl, relayDropRate,
  relayDropNoKey, relayDupSuppressed (inside the existing dedup-drop branch).
- HW test (3 boards, the marquee): A↔B, B↔C paired, A-C never paired (then physically
  out of range / `espnowrelayblock`), relay ON only at B → `espnowbroadcast` from A
  displays at C attributed to A's MAC; metrics show exactly one forward; triangle soak at
  ttl=10 with 20-message burst → each message displays exactly once per node, counters
  stop moving, stack HWM stable (observeHwm); rate-limit test; file transfer + bond stream
  concurrent with relay burst untouched.

### Phase 3 — Hardening + truth-telling docs (Effort S)
Rewrite the overpromising sections: docs/ESPNOW_MESH_SYSTEM.md star/TTL-theater sections,
USERGUIDE "espnowsend auto-routes via mesh" (it does not and still won't — broadcast-only),
help strings, Wire.h ttl comment ("hops remaining; decremented by flood relays;
relay-eligible: BCA TEXT/TIME_SYNC; ttl=1 = never forwarded"). Overnight 3-board soak as
the acceptance gate. gMeshSeen[24] (zero readers) deleted here or left to the pre-1.0
dead-code sweep — gV4Dedup's origin keying is the right primitive.

---

## 3. Cost summary (Phases 0-3)

- RAM: ~250 B PSRAM (forward buffer) + ~45 B DRAM (counters/limiter) + Phase 0's ~20 B.
  No new task, no new queue, no stack-budget change.
- Airtime: steady-state unchanged (heartbeats never relay). Event bursts: fully-meshed
  N=8 one text ≈ 125 ms aggregate worst case; N=16 ≈ 600 ms — event-driven and rate-capped.
- Wire format: **zero changes** (header untouched, no new opcodes; charter edits are
  comments only).
- Effort: S + S + M + S. Each phase is one plain-English versioned commit after the
  owner's HW pass, per house rules.

---

## 4. Phase R (future, Effort L) — routed unicast, the shelf architecture

Design C's S2/S3 survived red-team **as architecture** with three specced-wrong parts
fixed. Not scheduled; recorded so the next design session doesn't start over.

- **R1 (M) — route learning:** new opcode `PEER_LIST = 39` (charter-earmarked Discovery
  slot), broadcast-authed neighbor advertisement every 30 s, ttl=1 always (never relayed);
  132 B payload fits the 186-B BCA cap. Route table 32 × 20 B PSRAM: direct peers hops=1;
  2-hop routes from neighbor lists with metric = min(link RSSI EWMA) (needs Phase 0's
  RSSI threading); 90 s expiry, >3 dBm replacement hysteresis. `espnowmeshroutes` CLI.
  Red-team: prefer this over heartbeat piggybacking. Adaptive TTL could return here as
  `clamp(maxHopsInRouteTable, 1, meshTTL)` — a real algorithm this time.
- **R2 (L) — RELAY_DATA envelope + blind E2E unicast:** new opcode `RELAY_DATA = 5`
  (Transport band): outer 32-B header (BCA, outer ttl = hop budget, per-hop re-tag — same
  primitive as Phase 2) + 8-B `{finalDst[6], hopsUsed, rsv}` + **byte-identical inner
  frame** + 32-B outer tag. Inner payload ≤ 146 B, inner sealed plaintext ≤ 130 B
  (SessionConfirm 142 fits; sealed REKEY 146 is an exact fit — static_assert loudly).
  The relay is cryptographically blind; endpoints handshake through it
  (`espnowpairremote`), then E2E AEAD as usual.
  - **The keystone (best idea of the study):** at final delivery, re-enter
    `v4_try_handle_incoming(inner, synthetic recv_info{src_addr = inner->origin})`.
    The synthetic radio-src fixes *every* C3 structure at once — session lookup, isPaired,
    reassembly, ACK matching, the deferred src captures — with zero per-handler changes.
    E2E ACKs route back automatically, so ✓✓-Delivered becomes true end-to-end truth.
  - **Red-team blockers to fix before building R2:**
    1. Prerequisite refactor: funnel **every** `esp_now_send` site (send_frame,
       session_wrapped, encrypted_chunked, frag_ack, pair ctrl) through one
       radio-resolve/route primitive — the "single choke point in v4_send_frame" does not
       exist today (C4), and unfunneled sends to a non-peer far MAC fail with
       ESP_ERR_ESPNOW_NOT_FOUND.
    2. Restrict initial relayed traffic to **single-frame payloads (≤130 B plaintext)**:
       the reassembler stores fragments at fixed offset fragIndex×200 (:5350) — a 128-B
       routed stride corrupts it, and frag-ACKs (hand-rolled, radio-src-addressed) can't
       return over the route. Fragmented relay waits for a stride-aware gV4Reasm rework.
    3. Positive inner-frame whitelist; never relay: the entire bond band (RCE channel,
       triple-blocked), FILE_*/FS_* (exact-fit 202-B structs, pacing assumes 1 hop),
       STREAM (dedup-bypassed). Nested RELAY_DATA rejected, depth bound 2.
  - Far peers get a `routeFlags` byte in devices.json: no esp_now_add_peer, no gMeshPeers
    fan-out slot (heartbeats stay direct; far-peer freshness = route age).

---

## 5. Study provenance

Workflow run wf_d2997551-575 (9 agents, ~1.4M tokens): mappers wire-header / rx-dispatch /
tx-path / sessions-trust / scaffolding; designs A (minimal flood), B (routed V5 flag day —
rejected), C (staged hybrid — architecture adopted for Phase R); red-team confirmed A's
byte/airtime math and found the B/C blockers recorded above. Full transcripts under the
session's workflow directory.
