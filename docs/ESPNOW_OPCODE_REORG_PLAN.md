# ESP-NOW V4 Opcode Map & Reorganization Plan

> ## ⚡ DECISION — EXECUTED 2026-07-13
>
> The renumber was carried out the same day (user decision): **all categories are now
> 20 slots wide**, live opcodes compact from each range base, **no version bump and no
> migration code** — the whole fleet is erased and reflashed together. The authoritative
> map is the enum + charter comment in `System_ESPNow_Wire.h`; summary:
>
> ```
> 1–9     Transport   ACK=1 (2–4 reserved NACK/FRAG_REQ/FRAG_REPLY)
> 10–29   Crypto      KEY_EX 10–12, SESSION_OPEN/CONFIRM 13/14, REKEY 16→15
>                     (16/17 earmarked SESSION_AUTH_REQ/GRANT)
> 30–49   Discovery   HEARTBEAT 20→30, BOOT 21→31, TOPO 22-24→32-34,
>                     TIME_SYNC 25→35, PAIR_BEACON 26→36 (37–39 earmarked CAP/PEER_LIST)
> 50–69   App unicast CMD 30→50, CMD_RESP 31→51, TEXT 32→52, METADATA 33-35→53-55,
>                     USER_SYNC 36→56
> 70–89   Remote FS   FS_LIST/STAT/GET family 37-42→70-75 (76–81 earmarked write side)
> 90–109  Streaming   STREAM 50→90 (91–93 earmarked MEDIA_*)
> 110–129 Files       FILE_START/DATA/END 60-62→110-112, ACK/PROGRESS res 113/114,
>                     CANCEL 65→115, NACK/RESUME_OK res 116/117
> 130–149 Events      SUBSCRIBE_UPDATE 70→130, reserved 71-74→131-134
> 150–169 Sensors     SENSOR_BROADCAST 80→150, SENSOR_STATUS 82→151,
>                     POWER_STATUS res 152 (the old "84" plan)
> 170–189 Bond        90-97→170-177, plus STREAM_CTRL 51→178 (renamed
>                     BOND_STREAM_CTRL) and SENSOR_DATA 81→179 (renamed
>                     BOND_SENSOR_DATA) — moved in; they were always bond-gated
> 190–199 unallocated buffer · 200–255 user space (unchanged convention)
> ```
>
> The old retired holes (15, 83) are eliminated — the new map has no holes, and the
> "83 is poisoned" rule is obsolete. Version byte stays 4 (a mixed old/new fleet never
> exists because devices are erased before flashing; §6's mixed-fleet analysis is
> therefore historical context, not an active risk). Everything below this box is the
> audit and option analysis as written *before* the decision — reference values in §3
> are the OLD numbers.

**Status:** executed (see box above). Originally written 2026-07-13 as plan-only, from a full-code audit of
`components/hardwareone/System_ESPNow_Wire.h`, the RX dispatch path in
`System_ESPNow.cpp`, every TX call site, and the roadmap docs
(`ESPNOW_MESH_SYSTEM.md`, `AUTOMATION_ESPNOW_TRIGGERS_PLAN.md`,
`NOTIFICATION_EVENT_INTEGRATION_PLAN.md`, `SYSTEM_EVENT_BUS_PROMPT.md`,
`ESPNOW_PAIRING_MODE_PLAN.md`).

**Line-number caveat:** references were verified against the working tree on
2026-07-13 and will drift; function names are the stable anchors.

---

## 1. TL;DR

- The V4 opcode byte (`EspNowV4Header.type`, offset 3) is a `uint8_t` already organized
  into category ranges (1–9 transport … 90–99 bond, 200+ user). **45 opcodes are live**,
  9 more are named cold reservations, 2 are retired holes (15, 83).
- The real pressure points are **bond (90–99: 8 of 10 used)** and **discovery (20–29:
  7 of 10 used)**. Everything else has room, and **100–199 is a completely unallocated
  100-slot band**.
- Renumbering is far cheaper than it looks — the audit found **zero compiled numeric
  opcode uses** (everything is enum-symbol keyed), ACK/dedup/reassembly correlate by
  `msgId` not opcode, nothing persists opcode values, and the crypto is renumber-neutral.
  The entire cost is (a) a **fleet-wide flag-day reflash** and (b) **prose rot** in docs
  and comments.
- **Recommendation: don't renumber now.** Adopt **Option A** (§7): keep every live
  number, formally charter 100–199 as per-category overflow bands, commit numbers for the
  planned/likely opcodes, and write down the reservation policy. This yields 100+ usable
  slots, needs no version bump, no flag-day, and can be flashed device-by-device.
- **Option B** (§8) is a fully-specified clean-slate hex-aligned renumber (protocol
  version 4→5). Keep it in the drawer and execute it **only when some other change
  forces an incompatible wire break anyway** — the triggers are listed.

---

## 2. How the opcode byte works — and what does *not* depend on its value

Facts that bound every reorganization decision (all verified in code):

| Mechanism | Depends on opcode value? | Where |
|---|---|---|
| RX dispatch | **Symbol only** — linear scan of `kV4HandlerTable` (`v4_dispatch_lookup`) | `System_ESPNow.cpp:~4344` |
| Unknown opcode | Safe drop: `nullptr` lookup → DEBUG log → reassembly-slot cleanup → frame consumed | `System_ESPNow.cpp:~4982` |
| Version gate | Frames with `ver != ESPNOW_V4_VERSION` are rejected **before dispatch** | `System_ESPNow.cpp:4504` |
| ACK / delivery tracking | `msgId` (+ `fragIndex` for frag-ACKs), never opcode | `v4_send_ack`, ACK branch `~4817`, `broadcast_tracker_record_ack`, `sendStatusMarkDelivered` |
| Fragment reassembly | Keyed on (src MAC, `msgId`); stores type but never matches on it | `v4_reasm_find_or_alloc` |
| Dedup cache | (origin, `msgId`, `fragIndex`); opcode bypass list uses enum symbols | `v4_dedup_seen_and_insert` |
| Stats | Scalar totals only; **no per-opcode arrays** | `routerMetrics` |
| Persistence | **Nothing** — no opcode value in NVS, settings.json, or `identity.json` (`subscribedEvents` is an event-category bitmask, not opcodes); web JSON `type` is the separate `LogMessageType` storage enum | `System_ESPNow_Identity.cpp`, `WebPage_ESPNow.cpp` |
| Crypto | AEAD AAD (first 30 header bytes) and BROADCAST_AUTH HMAC cover the type **byte as wire bytes** — both ends recompile from the same enum, so verification is renumber-neutral. KEY_EX/SESSION signature transcripts don't include type at all. CRC16 covers payload only. | `System_ESPNow_Sessions.cpp:308–354`, `Handlers_Crypto.cpp` |
| Raw numeric literals in code | **None live.** One historic scar: CMD was once a hardcoded `5` that rotted when CMD moved 5→30 (killed all remote/bond commands); now the enum, with a warning comment | `System_Utils.cpp:~4234` |

So the opcode number is consequential in exactly **one** place: **the air between two
devices**. A renumber is a hard wire break between firmware versions; the fleet must be
reflashed together (acceptable per project policy — no backwards compat, owner erases
before flashing). §6 covers what a *transient* mixed fleet does.

The 16-bit `flags` field is a parallel namespace (ACK_REQ, SESSION_FRAME,
BROADCAST_AUTH, HANDSHAKE, STREAM_BEGIN/END…) and is not part of this plan, except to
note `ESPNOW_V4_FLAG_ENCRYPTED (0x0002)` is documented vestigial and its bit could
eventually be reclaimed by the same policy discipline as opcode holes.

---

## 3. Current inventory

Legend — **RX gate** is what the dispatch table + handler actually enforce (not what
comments claim). *P* = `REQ_PAIRED`, *B* = `REQ_BOND_MODE`, *A* = `REQ_AUTHENTICATED`
(session **or** broadcast-HMAC), *S* = `REQ_SESSION_ENC` (AEAD only). "flags=0" means
any radio in range can reach the handler (same-mesh fingerprint gate applies upstream).

### Transport 1–9 (1 live, 3 reserved, 5 free)

| # | Opcode | Status | RX gate | Notes |
|---|---|---|---|---|
| 1 | ACK | live | pre-dispatch, none | Handled *before* the table. Always plaintext (sent post-AEAD-verify for encrypted frames). Correlates by `msgId`/`fragIndex`. Spoofable: updates peer health, "✓✓ Delivered" marks, ping RTT unauthenticated. Excluded from reassembly (frag fields echo the acked frame). |
| 2–4 | NACK / FRAG_REQ / FRAG_REPLY | reserved (no enum symbols) | — | For future negotiated retransmit. |
| 5–9 | — | free | — | |

### Crypto / pairing 10–19 (6 live, 1 hole, 3 free)

All rows: table flags=0 by design — these opcodes *establish* trust; the handler does the
cryptographic verification (mesh-bootstrap-key HMAC for KEY_EX, Ed25519 against the
stored long-term pubkey for SESSION_*). SESSION_OPEN/CONFIRM/REKEY defer heavy crypto to
`cmd_exec`.

| # | Opcode | Status | Notes |
|---|---|---|---|
| 10 | KEY_EX_HELLO | live | 3-way pairing handshake, plaintext + `FLAG_HANDSHAKE`. |
| 11 | KEY_EX_REPLY | live | Sent from the HELLO RX handler only if HMAC verified. |
| 12 | KEY_EX_CONFIRM | live | Status byte + pubkey fingerprint for OOB display. |
| 13 | SESSION_OPEN | live | SIGMA-I signed ephemeral DH. Auto-initiated by heartbeat pre-warm (3-strike backoff). |
| 14 | SESSION_CONFIRM | live | Sent from the deferred SESSION_OPEN worker. |
| 15 | *(ex-SESSION_CLOSE)* | **retired hole** | Never sent/handled; removed 2026-06. Stale row still in `ESPNOW_ARCHITECTURE.md` opcode table — scrub before any reuse. |
| 16 | SESSION_REKEY | live | Threshold-triggered (txSeq count / session age); signed, sent plaintext outside the session. |
| 17–19 | — | free | |

### Discovery / timing 20–29 (7 live, 3 free) ← **second-tightest range**

| # | Opcode | Status | RX gate | Notes |
|---|---|---|---|---|
| 20 | HEARTBEAT | live | flags=0; backup-master failover logic additionally requires authenticated frames | 5 s per-peer fan-out (BROADCAST_AUTH HMAC) + master→backup unicast (AEAD) + app-page ping reuse. Unauthenticated heartbeats still count toward liveness. One free payload byte earmarked for battery% (mesh report rank 4). |
| 21 | BOOT | live | flags=0 | Boot/online notice; shares `v4h_text` but files as system event, not chat. |
| 22 | TOPO_REQ | live | flags=0 | CLI `espnowtopology`. Entire topology exchange is plaintext + ungated: any same-mesh radio can enumerate membership. (`v4_broadcast_topo_request`/`v4_send_topo_request` are dead code.) |
| 23 | TOPO_START | live | flags=0 | Reply leg, `reqId`-correlated (guessable while window open). |
| 24 | TOPO_PEER | live | flags=0 | Peer MAC + RSSI + name in clear. |
| 25 | TIME_SYNC | live | **A** | Moves the device clock — poster child for REQ_AUTHENTICATED. Manual CLI, per-peer fan-out with BROADCAST_AUTH. |
| 26 | PAIR_BEACON | live | **A** + own-window-open + not-paired + cooldown | The **only true FF:FF broadcast** opcode. WPS-style mutual pairing (HW-validated 2026-07-07). |
| 27–29 | — | free | | `ESPNOW_PAIRING_MODE_PLAN.md` calls 26–29 "FREE"; 26 is now taken. |

### App unicast 30–49 (13 live, 7 free)

| # | Opcode | Status | RX gate | Notes |
|---|---|---|---|---|
| 30 | CMD | live | **P**; handler hard-rejects non-AEAD frames and requires credentials (`@BOND:` token or user:pass) | Effective policy = paired + session-encrypted + authenticated. Single deferred slot (a 2nd CMD before execution is dropped). |
| 31 | CMD_RESP | live | flags=0 (**plaintext RX accepted, `msgId`-correlated — spoofing seam**) | All firmware TX paths are AEAD. Reuses the CMD's `msgId`, so it's on the dedup bypass list. Many TX sites (final/failure results, per-error rejections, ~12 USER_SYNC status replies via the shared helper). |
| 32 | TEXT | live | flags=0 (chat from unpaired senders accepted, MAC as name) | Mixed TX: `espnowsend` unicast = AEAD; `espnowbroadcast` = authenticated-plaintext fan-out. Multi-frame TEXT deliberately bypasses device-side reassembly. `v4_send_text()` is dead code. |
| 33 | METADATA_REQ | live | flags=0 | Reply is strict-encrypt, so ungated RX leaks nothing to unpaired radios. |
| 34 | METADATA_RESP | live | flags=0 | TX strict encrypt-or-queue (2026-05 fix: was plaintext, leaked room/zone/tags). |
| 35 | METADATA_PUSH | live-**but-dead-on-wire** | flags=0 | RX fully plumbed, TX half-plumbed (`sendMetadata(isPush)`) but **no caller ever passes `isPush=true`** — this opcode never appears on the wire. Wire up (push on metadata change) or retire. |
| 36 | USER_SYNC | live | flags=0 at table; deferred worker requires session-encrypted + setting enabled + embedded receiver-admin credentials | Carries four credentials in one JSON — strict encrypt both ways. Historical footgun: gating on the vestigial `FLAG_ENCRYPTED` bit broke it. |
| 37–42 | FS_LIST_REQ/REPLY, FS_STAT_REQ/REPLY, FS_GET_REQ/ACK | live | **P+S** (uniform) | Base ESP-NOW capability (deliberately not bond-gated). Structured replacement for CLI scrapes. FS_GET triggers a FILE_* transfer for the payload. |
| 43–49 | — | free | | Natural home for FS **write-side** ops (`V4PayloadFsEntry.perms` already carries WRITE/DELETE bits for exactly this). |

### Streaming 50–59 (2 live, 8 free)

| # | Opcode | Status | RX gate | Notes |
|---|---|---|---|---|
| 50 | STREAM | live | flags=0 | Live command output, `msgId`=cmd's; dedup-bypassed; STREAM_BEGIN/END flags. TX is AEAD. |
| 51 | STREAM_CTRL | live | **P+B+S** | **Policy outlier in this range:** bond-only (master→worker sensor stream start/stop), compiled under `ENABLE_BONDED_MODE`. Numerically "streaming", behaviorally bond. |
| 52–59 | — | free | | Media streaming (camera/mic) would land here if the null-builder streams ever go live. |

### Files 60–69 (4 live, 2 reserved, 4 free)

| # | Opcode | Status | RX gate | Notes |
|---|---|---|---|---|
| 60 | FILE_START | live | **P** (not session-gated on RX; TX always encrypt-or-queue — the Wire.h "plaintext otherwise" comment is stale) | `canRead()` ACL on the sender blocks credential files. |
| 61 | FILE_DATA | live | **P** | 200 B/chunk; dedup-bypassed (`msgId` = transferId). 343 KB/1717 chunks HW-validated. |
| 62 | FILE_END | live | **P** | CRC32 of the whole file. **Doubles as the bond RESP channel**: settings/schema/manifest replies arrive as FILE_END for magic filenames (`_settings_out.json`, `_schema_out.json`, `_manifest_out.json`, `automations.json`) — that's why V3's MANIFEST_RESP/SETTINGS_RESP/SETTINGS_PUSH have no V4 numbers. Known inline-heavy handler exception. |
| 63 | FILE_ACK | reserved | — | Phase 4 follow-up (windowed ack/backpressure). |
| 64 | FILE_PROGRESS | reserved | — | Phase 4 follow-up (fixes long no-feedback transfers). |
| 65 | FILE_CANCEL | live | **P** | Receiver→sender post-hoc failure notice (TIMEOUT/WRITE_FAILED/INCOMPLETE); plaintext control frame like ACK. |
| 66–69 | — | free | | FILE_NACK (missing-chunk bitmap) + resume marker land here (mesh report rank 12). |

### Events 70–79 (1 live, 4 reserved, 5 free)

| # | Opcode | Status | RX gate | Notes |
|---|---|---|---|---|
| 70 | SUBSCRIBE_UPDATE | live | **P** (not session-gated despite Wire.h "preferred" note) | Narrows-only category bitmap; gates `v4_broadcast_category` fan-outs. |
| 71–74 | UNSUBSCRIBE_ALL, EVENT_PUSH, SUB_LIST_REQ, SUB_LIST_REPLY | reserved | — | Phase 5 completion. EVENT_PUSH is the likely carrier for mesh propagation of the new `System_Events` kinds. |
| 75–79 | — | free | | A future semantic-kind subscribe (mask wider than 32 bits) would land here. |

### Sensors 80–89 (3 live, 1 poisoned hole, 6 free)

| # | Opcode | Status | RX gate | Notes |
|---|---|---|---|---|
| 80 | SENSOR_BROADCAST | live | flags=0 (**anyone in range can inject fake remote readings — display-only cache**) | Per-peer fan-out, BROADCAST_AUTH on TX (auth-only by Phase 3.5 decision). |
| 81 | SENSOR_DATA | live | **P+B+S** | **Policy outlier:** bond-only binary streaming (worker→master), `ENABLE_BONDED_MODE`. |
| 82 | SENSOR_STATUS | live | flags=0 | Edge-triggered enable/disable announcements. |
| 83 | *(ex-WORKER_STATUS)* | **retired hole — poisoned** | — | Sender removed 2026-05-21, opcode 2026-06. Multiple docs cite "opcode 83"; mesh report explicitly numbers the future POWER beacon **84** to route around it. Never reuse. |
| 84–89 | — | free | | 84 = POWER_STATUS (planned, mesh report rank 8); mesh-legal sensor pull / envelope replies would follow. |

### Bond 90–99 (8 live, 2 free) ← **tightest range**

All rows: **P+B+S**, compiled only under `ENABLE_BONDED_MODE`; senders go through
`bondSendEncryptedAsync` (single-initiator + encrypt-or-wait). Plaintext bond frames are
dropped loudly.

| # | Opcode | Notes |
|---|---|---|
| 90 | BOND_HEARTBEAT | Carries seqNum/bootCounter/settingsHash (drives settings-resync detection). |
| 91 | BOND_CAP_REQ | Bond sync tick + `bondrequestcap`/`bondresync`. |
| 92 | BOND_CAP_RESP | Caches remote capability, keys manifest cache by fwHash. |
| 93 | MANIFEST_REQ | Response = FILE_* transfer of `_manifest_out.json` (no RESP opcode). |
| 94 | SETTINGS_REQ | Response = FILE_* of `_settings_out.json` (+ `automations.json`). |
| 95 | BOND_STATUS_REQ | Internal bond machinery (no direct CLI). |
| 96 | BOND_STATUS_RESP | Feeds the bonded-device status panel. |
| 97 | SCHEMA_REQ | Response = FILE_* of `_schema_out.json`. Highest assigned value. |
| 98–99 | free | Only 2 slots left in the category. |

### 100–199: **completely unallocated** (100 slots). 200–255: "user-defined, reserved for plugin-style extensions" (comment-only convention; nothing uses it).

---

## 4. Occupancy & pressure analysis

```
range      used  reserved  holes  free   pressure
1–9    Tx    1      3        0      5    low
10–19  Cry   6      0        1      3    low (17–19 could take session-auth pair)
20–29  Dis   7      0        0      3    HIGH — 3 slots, ≥3 candidates (CAP_REQ/RESP, PEER_LIST)
30–49  App  13      0        0      7    medium (FS write-side wants ~4–6)
50–59  Str   2      0        0      8    low
60–69  Fil   4      2        0      4    medium (resume/NACK work is roadmapped)
70–79  Evt   1      4        0      5    low-medium (event-bus mesh propagation)
80–89  Sen   3      0        1      6    low-medium (POWER=84 planned)
90–99  Bnd   8      0        0      2    HIGH — fullest category
100–199      0      0        0    100    ← the actual answer to "where's the space"
200–255      0      0        0     56    convention-only user space
```

Structural findings beyond raw counts:

1. **Range ≠ policy in two places.** STREAM_CTRL (51) and SENSOR_DATA (81) are
   bond-gated (`P+B+S`, `ENABLE_BONDED_MODE`) but live in the streaming/sensor ranges.
   Everything else in a range shares one effective gate tier. Any reorg should either
   move them (Option B) or document them as exceptions (Option A).
2. **Two holes with different rules.** 15 is reclaimable after scrubbing the stale doc
   row; 83 is permanently poisoned (multiple doc citations; the roadmap already routes
   around it).
3. **Dead-on-wire / dead code.** METADATA_PUSH (35) never transmits; `v4_send_text` has
   no callers, and `v4_broadcast_topo_request` (+ `v4_send_topo_request`, reachable only
   through it) is transitively dead. Retire or wire up — either way they distort the
   "used" count.
4. **No opcode→name debug table exists.** Logs print raw numbers (`Type=%u`). Any
   renumber makes historical logs ambiguous; a name-lookup helper would help both
   options (and is cheap flash).
5. **The request/response pattern costs two slots each time** (6 of the 13 app-unicast
   slots are the FS family). Future protocol families should budget pairs, which is why
   generous per-category spacing matters more than total free count.

---

## 5. Future opcode demand (from the plan docs)

| Confidence | Feature | New opcodes | Lands in | Source |
|---|---|---|---|---|
| planned | POWER beacon (voltage/charging/USB/heap) | 1 (**84** — explicitly chosen to skip poisoned 83) | sensors | ESPNOW_MESH_SYSTEM §5.2 rank 8 |
| planned | File-transfer resume / selective retransmit (missing-chunk bitmap) | ~2 (FILE_NACK + resume marker) | files | ESPNOW_MESH_SYSTEM §5.2 rank 12, §2.7 |
| planned | Battery in HEARTBEAT; CMD_RESP success/resultClass | **0** (payload-only changes) | — | ESPNOW_MESH_SYSTEM ranks 2–6; AUTOMATION_ESPNOW_TRIGGERS_PLAN |
| planned | Notification/event integration plan | **0** (all sinks are on-device) | — | NOTIFICATION_EVENT_INTEGRATION_PLAN |
| likely | FILE_ACK (63) + FILE_PROGRESS (64) activation | 0 new (numbers pre-reserved) | files | Wire.h comment + mesh report |
| likely | Phase-5 events completion (71–74) + EVENT_PUSH carrying `System_Events` kinds over mesh | 0 new (numbers pre-reserved) | events | Wire.h + System_Events.h |
| likely | FS write-side (delete, put, mkdir, rename + acks) | ~4–6 | app unicast 43–49 | `V4PayloadFsEntry.perms` WRITE/DELETE bits |
| likely | Session-scoped auth (`espnowauth` / `@SESSION:` credential-free remote exec) | ~2 | crypto 17–18 | ESPNOW_MESH_SYSTEM §5.2 rank 11 |
| speculative | Mesh capability discovery (CAP_REQ/RESP), peer-directory gossip for worker-side ONLINEPEERS/ONLINEROOMS | ~3 | discovery 27–29 (**fills the range**) | ESPNOW_MESH_SYSTEM §2.5; AUTOMATION_ESPNOW_TRIGGERS_PLAN §A4 |
| speculative | Mesh-legal sensor pull (STREAM_CTRL analog outside bond), envelope replies | ~2 | sensors 85–86 | ESPNOW_MESH_SYSTEM §2.6; SENSOR_READING_ENVELOPE_PLAN |
| speculative | Semantic-kind event subscribe v2 (>32-bit mask) | ~1 | events 75 | V4PayloadSubscribe reserved bytes too small |
| speculative | Media streaming (camera/mic frames) | ~3 | streaming 52+ | null-builder stream registry |
| speculative | Bond growth (power status, settings-sync acks, bond event push) | ~2–3 | bond — **only 98–99 left** | range pressure |
| speculative | Automation-to-automation signaling / user events | ~8–16 | 200+ user space | AUTOMATION_TRIGGERS_EXPANSION extrapolation |
| speculative | Multi-hop relay (ROUTE_*) | ~2 | new category | mesh report documents relay as absent (TTL is inert) and rejects TTL-tuning in §5.3; nothing roadmaps building it — do not reserve prime space |

Net: **planned+likely demand fits existing free slots everywhere except bond**, and
discovery hits zero if all three speculative candidates land. Those two categories are
what the reorganization must actually solve.

---

## 6. What renumbering actually costs (downstream impacts)

| Area | Impact | Severity |
|---|---|---|
| Over-the-air compat | Opcode byte crosses to peer devices → renumber = hard wire break; whole fleet reflashes together | **the** cost |
| Transient mixed fleet (no version bump) | See below — real misinterpretation hazard | **blocker if unmitigated** |
| Transient mixed fleet (with 4→5 bump) | Pre-dispatch `ver` gate (`System_ESPNow.cpp:4504`) rejects foreign frames → the two fleets are **mutually silent**, not mutually misparsing | fail-safe |
| Compiled code | Enum-symbol-keyed everywhere; recompile handles it. No numeric switch/range/array/JS uses exist | none |
| ACK/retry, dedup, reassembly, stats | `msgId`-keyed, opcode-agnostic | none |
| Persistence (NVS/settings/identity.json) | No opcode values stored anywhere. One future caveat: the SD frame-capture files log raw `TYPE=%u` numerals — vacuously empty today because of the `ver != 3` bug below, but once fixed, capture files from before a renumber read ambiguously (cosmetic; they're debug artifacts) | none |
| Crypto (AEAD AAD, BROADCAST_AUTH HMAC, signatures) | Type is bound as literal wire bytes; both ends recompile together → neutral. No key/format changes | none |
| OLED/G2/web "clients" | Same single firmware binary, shared enum | none (recompile) |
| Docs & comments | **Where the entire migration risk concentrates**: `ESPNOW_ARCHITECTURE.md:104–117` (numeric opcode table — both stale [15/83 rows] *and* incomplete [missing BOOT, PAIR_BEACON, FS 37–42, FILE_CANCEL, SCHEMA_REQ]), `:231`; `ESPNOW_MESH_SYSTEM.md` (:41, :129, :136, :284 numeric + :288, :334 Wire.h line refs); `ESPNOW_PAIRING_MODE_PLAN.md` (4 cites); `ESPNOW_HELPTEXT_AUDIT.md:19`; `SYSTEM_EVENT_BUS_PROMPT.md:145–146`; `ESPNOW_ARCHITECTURE_DIAGRAMS.html:176`; Wire.h prose; `System_ESPNow.cpp:2371`, `System_ESPNow_Identity.h:111–117`, `System_ESPNow.h` (3 comment refs), `System_Utils.cpp:~4234` comments | mechanical but easy to miss |
| Handler-table edit risk | A dropped `kV4HandlerTable` row compiles clean and fails **silent** (unknown-type DEBUG drop) — the mesh report's recurring failure shape | must-verify on HW |

**The mixed-fleet hazard, concretely.** An unknown opcode is dropped safely. The danger
is *collision*: old firmware looks the new number up in *its* table and finds a
different handler. Gating flags and payload-size checks catch most cases (and
memory-safety always holds — every handler size-checks), but not all:
TIME_SYNC and PAIR_BEACON are both authenticated per-peer/FF broadcasts carrying a valid
mesh-group-key HMAC. If a renumber ever mapped one firmware's PAIR_BEACON onto the
other's TIME_SYNC value, the beacon (24 B) passes `REQ_AUTHENTICATED`, exceeds
`sizeof(V4PayloadTimeSync)` (16 B), and `v4h_time_sync` applies
`gTimeOffset = garbage; gTimeIsSynced = true` — **clock corruption from a frame the
receiver considers fully valid**. The auth gate can't help because both opcodes carry
legitimate HMACs; only the *number* distinguishes them, and the number is what changed.
This is why any renumber must bump `ESPNOW_V4_VERSION` — and why the historic CMD 5→30
renumber (which broke all remote commands via a rotted literal) is remembered in a code
comment as a warning.

**Adding opcodes never needs any of this.** New number + new handler row; old firmware
unknown-type-drops it. No version bump, no flag-day, flash devices in any order.

**Pre-existing bug found by this audit (fix independently of any reorg):**
`captureEspNowFrame` gates on `h->ver != 3` — a rotted V3 literal at
`System_ESPNow.cpp:5377` — so the SD frame-capture feature (`espnowCaptureToSd`)
silently drops **every** live V4 frame today. Change to `ESPNOW_V4_VERSION`.

---

## 7. Option A (recommended): keep all numbers, charter the space

Every live opcode keeps its exact number. Headroom comes from formalizing what is
currently informal:

### 7.1 The map

```
base range          overflow band   policy default for NEW rows
1–9    transport    101–109         pre-dispatch/plaintext-legal, msgId-correlated
10–19  crypto       110–119         flags=0 + mandatory in-handler crypto verification
20–29  discovery    120–129         REQ_AUTHENTICATED minimum (TIME_SYNC/PAIR_BEACON tier;
                                    legacy 20–24 plaintext rows are a documented gap, not the template)
30–49  app unicast  130–149         REQ_PAIRED|REQ_SESSION_ENC (the FS-family standard;
                                    legacy 30–36 documented as-is)
50–59  streaming    150–159         REQ_PAIRED|REQ_SESSION_ENC
60–69  files        160–169         REQ_PAIRED min., TX via smart encrypt-or-queue
70–79  events       170–179         REQ_PAIRED, session-encryption for state-mutating rows
80–89  sensors      180–189         BROADCAST_AUTH fan-out / P+S for pull-style rows
90–99  bond         190–199         REQ_PAIRED|REQ_BOND_MODE|REQ_SESSION_ENC, no exceptions
100                 —               never-assign sentinel (mirror of 0; keeps +100 arithmetic clean)
200–255             —               user space, untouched; core never allocates above 199
```

Each overflow band inherits its base category's security default, so the number alone
still tells a reviewer the gating contract: **opcode `1xy` ⇒ category `xy`**. Bond gets
190–199 (2 → 12 free slots) and discovery gets 120–129 — the two measured pressure
points — without moving anything.

### 7.2 Committed numbers for roadmapped work

Recorded as comment reservations in the enum (same style as today's 63/64); the enum
symbol + `kV4HandlerTable` row + handler must ship together in one change:

| # | Name | Trigger |
|---|---|---|
| 17/18 | SESSION_AUTH_REQ / SESSION_AUTH_GRANT | espnowauth work (rank 11) |
| 43/44 | FS_DELETE_REQ / FS_DELETE_ACK | first FS write-side op (45–47 earmarked PUT/MKDIR/RENAME) |
| 63/64 | FILE_ACK / FILE_PROGRESS | already reserved; transfer-feedback work |
| 66/67 | FILE_NACK / FILE_RESUME_OK | resume/selective-retransmit (rank 12) |
| 71–74 | UNSUBSCRIBE_ALL / EVENT_PUSH / SUB_LIST_REQ / SUB_LIST_REPLY | already reserved; Phase 5 + event-bus mesh propagation |
| 84 | POWER_STATUS | power beacon (rank 8; 83 stays dead) |
| 27–29 | CAP_REQ / CAP_RESP / PEER_LIST | earmark only (speculative) |

### 7.3 Reservation policy (goes into the Wire.h range-map comment as source of truth)

- **P1.** New opcodes draw from the owning category's base range first, then its +100
  band; never from another category's range. The Wire.h range map is updated in the
  same commit as any enum change.
- **P2.** An enum symbol is added only when its dispatch row + handler ship in the same
  change (a TX'd opcode with no handler is a silent drop — the known failure shape).
  Speculative ideas stay comment-only earmarks, re-justified or released at the next
  allocation in that category.
- **P3.** Retired numbers become documented holes. Reuse requires scrubbing every stale
  doc/comment reference in the same commit (15 qualifies after a doc pass; **83 never**).
- **P4.** *Adding* opcodes never bumps `ESPNOW_V4_VERSION`. *Renumbering* any existing
  opcode, or incompatibly changing an existing payload struct, **requires** the bump
  (mutually-silent beats mutually-misparsing).
- **P5.** Every new dispatch row states its gate flags explicitly; deviating from the
  category default requires a justification comment.
- **P6.** 0 and 100 are never-assign; 200+ is user space; core stops at 199.

### 7.4 Why this beats renumbering today

- Delivers all measured headroom (bond 12, discovery 13, ~135 free core slots) for a
  **comment-level change** — zero wire impact, zero flag-day, flash at leisure.
- The clean-slate scheme's tangible wins are mnemonic (hex nibbles in dumps) — but
  nothing in the codebase does numeric range checks, there's no sniffer tooling, and
  the SD capture path is currently broken anyway.
- Renumbering stays available for free later: P4 means the *next* forced wire break can
  carry the renumber as a passenger.

Residual risks: the mirror-band policy is convention, not mechanism (a careless
`flags=0` row in 120–129 would reopen the plaintext-gap class — mitigable with a
boot-time table audit asserting per-range minimum flags); earmarks can rot like 63/64
did (P2's review rule is the counterweight); and this is explicitly **not** a security
pass — the plaintext-tolerant legacy rows (topology, CMD_RESP spoof seam, ACK spoof)
keep their gaps and deserve their own follow-up.

---

## 8. Option B (in the drawer): clean-slate V5 hex map

A fully-specified renumber for when a wire break happens anyway. Protocol version bumps
4→5 in the same commit (mandatory — see §6). Ranges are hex-aligned 16-wide blocks: the
high nibble *is* the category (`type & 0xF0`), conceptual ordering preserved.

```
0x01–0x0F transport      ACK=0x01; NACK/FRAG_REQ/FRAG_REPLY 0x02–0x04         (11 free)
0x10–0x1F crypto/session KEY_EX 0x10–0x12; SESSION_OPEN/CONFIRM 0x14/0x15;
                         REKEY 0x16; earmark SESSION_AUTH 0x18/0x19            (8 free)
0x20–0x2F discovery      HEARTBEAT 0x20 … PAIR_BEACON 0x26 (order kept);
                         earmark CAP_REQ/RESP 0x28/0x29, PEER_LIST 0x2A        (6 free)
0x30–0x3F app messaging  CMD 0x30, CMD_RESP 0x31, TEXT 0x32, METADATA 0x33–0x35,
                         USER_SYNC 0x36; earmark LLM_ASK 0x38                  (8 free)
0x40–0x4F remote FS      FS_LIST/STAT/GET family 0x40–0x45; earmark write-side
                         PUT/DELETE/MKDIR/RENAME 0x46–0x4B                     (4 free)
0x50–0x5F file transfer  FILE_START/DATA/END 0x50–0x52, FILE_ACK/PROGRESS
                         0x53/0x54, FILE_CANCEL 0x55; earmark NACK/RESUME      (8 free)
0x60–0x6F streaming/media STREAM 0x60; earmark MEDIA_START/DATA/END           (12 free)
0x70–0x7F events         SUBSCRIBE_UPDATE 0x70, reserved 0x71–0x74, EVENT_SUB v2 (10 free)
0x80–0x8F sensors (mesh) SENSOR_BROADCAST 0x80, SENSOR_STATUS 0x81; earmark
                         POWER_STATUS 0x82, SENSOR_REQ/ENVELOPE                (11 free)
0x90–0xBF structural reserve — 48 contiguous slots for whole new categories
0xC0–0xDF bond (32 slots) BOND_* 0xC0–0xC7 + STREAM_CTRL→0xC8 and
                         SENSOR_DATA→0xC9 **moved in** (rename BOND_STREAM_CTRL /
                         BOND_SENSOR_DATA); one contiguous ENABLE_BONDED_MODE block (19+ free)
0xE0–0xFE user/plugin (31 slots; the old "200+" convention moves — decimal 200 = 0xC8
                         would otherwise collide with bond)
0xFF      reserved escape (static_assert; future two-byte extended-opcode hatch)
```

Structural wins over Option A: every range gets exactly one gate policy (the two
outliers move into bond), the bond `#ifdef` collapses to one block, ranges are sized by
measured demand (bond 2→22 free), and holes 15/83 are deleted outright (the ver-5 gate
makes number reuse safe). Retired-hole bookkeeping disappears.

Migration is a **single atomic commit**: rewrite the enum (+ named range-base constants,
`static_assert` no enumerator == 0xFF), bump the version, fix the `ver != 3` capture
literal, reorder the handler table (symbol-keyed — verify no row is lost; a lost row
fails silent), sweep the comment/doc numerals (§6 list), grep for stale numerals
(the CMD 5→30 lesson), build for FeatherS3 + `HW_BOARD=xiao_s3` + a bonded config, then
reflash the whole fleet in one session and run one exercise per handler-table block
(pairing, TIME_SYNC, heartbeat presence, CMD both credential forms, TEXT both ways,
metadata, FS browse/stat/get, a multi-hundred-KB FILE transfer, STREAM, bond
status/cap/schema).

**Execute Option B only when one of these happens anyway:**
- an incompatible header or existing-payload change forces a version bump on its own;
- sniffer/frame-capture tooling becomes real (nibble-aligned ranges start paying rent);
- a third policy-outlier opcode appears (evidence the feature/security split is rotting);
- bond genuinely exhausts 98/99 + 190–199 under Option A.

---

## 9. Suggested sequencing

1. **Now (this doc's follow-up, no wire impact):** rewrite the Wire.h range-map comment
   to the Option A charter (§7.1 + §7.3), including committed numbers (§7.2), the
   83-poisoned / 15-doc-gated annotations, and the 0/100 sentinels.
2. **Same pass, opportunistic fixes:** `ver != 3` → `ESPNOW_V4_VERSION` in
   `captureEspNowFrame` (real bug — SD capture is a no-op today); fix the opcode table
   in `ESPNOW_ARCHITECTURE.md:104–117` (stale 15/83 rows, *and* missing live rows:
   BOOT 21, PAIR_BEACON 26, FS 37–42, FILE_CANCEL 65, SCHEMA_REQ 97); update
   `ESPNOW_PAIRING_MODE_PLAN.md`'s "26–29 are FREE"; optionally delete the dead
   `v4_send_text` / `v4_broadcast_topo_request` helpers and decide METADATA_PUSH's fate.
3. **Optional cheap hardening:** a boot-time (debug-build) walk of `kV4HandlerTable`
   asserting per-range minimum gate flags; a small opcode→name lookup for logs.
4. **As features ship:** allocate per §7.2/P2 — symbol, table row, and handler in one
   change; no version bump needed, devices flash in any order.
5. **Someday, bundled with a forced wire break:** execute Option B from the drawer.

---

## Appendix A: every reference surface (what a renumber must touch)

Compiled code (symbol-keyed — recompile only): `System_ESPNow_Wire.h` (enum + payload
structs), `System_ESPNow.cpp` (~130 refs: dispatch table, handlers, TX helpers, dedup
bypass, unpaired-reject telemetry), `System_ESPNow_FsList.cpp`,
`System_ESPNow_Handlers_Crypto.cpp`, `System_Utils.cpp` (remote-`@device` CMD send),
OLED/G2/web pages (via shared helpers).

Prose that cites numbers (rot risk): `ESPNOW_ARCHITECTURE.md` (numeric table :104–117 —
stale *and* incomplete, see §6; plus :231), `ESPNOW_MESH_SYSTEM.md` (:41, :129, :136,
:284 numeric; :288, :334 are Wire.h *line* refs that rot under an enum rewrite),
`ESPNOW_PAIRING_MODE_PLAN.md` (:69, :155, :243, :437), `ESPNOW_HELPTEXT_AUDIT.md` (:19),
`SYSTEM_EVENT_BUS_PROMPT.md` (:145–146), `ESPNOW_ARCHITECTURE_DIAGRAMS.html` (:176),
Wire.h header prose (version byte, range map, size-cap guidance naming
"30–49 / 60–69 / 80–89"), comments at `System_ESPNow.cpp:2371`,
`System_ESPNow_Identity.h:111–117`, `System_ESPNow.h` (:543, :1047, :1111),
`System_ESPNow_Tx.h:89`, `System_ESPNow_Sensors.cpp:529`, `System_Utils.cpp:~4234`.

Nothing else: no NVS/flash persistence, no web-JS numerals, no external tools, no
companion-app coupling (ESP-NOW is device↔device only).
