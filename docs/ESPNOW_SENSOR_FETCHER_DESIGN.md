# ESP-NOW Secure Sensor Fetcher — Design

> **Implementation status (2026-07-23):** Phase 0, Phase 1, and the Phase 1b test CLI
> (`espnowsensorreq`) are implemented and build green on FeatherS3 (esp32s3). Phase 2 (the
> master 30 s auto-fetch tick), Phase 3 (failover), and Phase 4 (history sink) are NOT yet
> implemented. An adversarial implementation review confirmed the four security invariants
> (D1/D2/fingerprint-auth/H3) hold in the written code, and caught two bugs that were then
> fixed:
> - **BUG-1 (fixed):** `leaseMs`/`intervalMs` were `uint16_t` (max 65535 ms) — a 90 000 ms
>   lease truncated to 24 464 ms. Widened to `uint32_t` on the wire (`V4PayloadSensorReq`),
>   in `EspNowState`, in `espnowApplySensorSubscription`, and in `gSensorReqIntervalMs[]`.
> - **BUG-2 (fixed):** the H3 in-task self-teardown could race an external
>   `stopSensorBroadcaster` and double-`vTaskDelete` the same handle. Fixed with an atomic
>   handle-claim (`claimBroadcasterForDelete` under a `portMUX`): exactly one actor deletes;
>   the loser parks until deleted.
>
> Deferred LOW items (not blocking, noted for later): D1 rejects `SOURCE_ESPNOW` only — an
> MQTT/BLE-bridged `espnowsensorstream` is not blocked (matches the spec's ORIGIN_ESPNOW
> wording, narrower than "local-console-only" prose); `gSensorControllerMac` is not cleared
> on lease lapse (a later local manual stream would target the stale controller — still AEAD
> to a paired peer, no confidentiality break); `v4h_sensor_envelope` ingest is not
> fingerprint-checked (a paired peer can only write readings under its own cryptographically-
> bound source MAC — an authenticity parity with the old broadcast, not a new hole).

Status: design, ready to implement (post adversarial security review)
Author: firmware architecture
Scope: automated, secure, master-orchestrated mesh sensor collection over ESP-NOW V4

This document specifies a **master-orchestrated secure subscribe-with-lease** sensor
fetcher for the HardwareOne ESP-NOW mesh. Every design claim below is grounded in the
current tree at `file:line`. It is design-only; the code sketches are struct field
lists, not implementations. An adversarial security pass (verdict: *sound with fixes*)
found three high-severity gaps in the first draft — a broader-than-intended remote control
path, a plaintext data path, and a self-deleting-task footgun — plus two low-severity
threat-model completeness items. All are folded into the sections below.

Two of those were then **hardened past the review's own recommendation**, by explicit
maintainer decision:

- **D1 — one remote control path.** `SENSOR_REQ` (fingerprint-gated to master/backup) is
  the *only* remote way to control worker streaming; the pre-existing `espnowsensorstream`
  command is made local-console-only, closing the generic credentialed remote-exec route to
  sensor control entirely (rather than merely gating it).
- **D2 — one data path, always encrypted.** The plaintext `SENSOR_BROADCAST = 150` mesh path
  is **removed**; encrypted-unicast `SENSOR_ENVELOPE` is the only way sensor data ever leaves
  a worker (rather than keeping plaintext for "local/manual" use).

The final section (§11) summarizes what the review found and how the design now closes each,
including these two decisions.

---

## 1. Overview & goals

A designated **MASTER** node periodically renews a short-lived **lease** on each ALIVE
paired **WORKER** in the mesh, instructing it to stream a chosen set of sensors. The
worker self-paces its pushes through the *existing* `sensorBroadcasterTask`
(`System_ESPNow_Sensors.cpp:571`) and **auto-stops** the moment its lease lapses. There
is no per-cycle polling and no boot-time auto-streaming: a worker that hears nothing
from a trusted master simply stays silent. Authorization is crypto-anchored to
`securepair` (Ed25519), narrowly scoped to "control my sensor streaming," and bound to
**two** specific controllers (primary master + backup master) so failover needs no
worker reconfiguration.

Decided model (do not re-litigate):

- **Transport:** subscribe + lease, NOT poll-each-cycle. Master sends/renews a
  subscription (`stream sensors {mask}, lease ~90s`) on a ~30s tick.
- **Self-pacing:** worker streams via the existing `sensorBroadcasterTask`. Lease expiry
  is handled **in-task** by clearing the sensor's streaming flag; when the last leased
  sensor lapses the broadcaster performs a **clean self-teardown** (nulls its own handle
  first, then `vTaskDelete(NULL)`). It must **not** call `stopSensorBroadcaster()` on
  itself — that would self-delete via its own cached handle and wedge the worker until
  reboot (§4, security review issue H3).
- **Trust anchor:** `securepair`. A worker honors sensor-control only from a peer that is
  (a) securely PAIRED — its Ed25519 pubkey is on file via KEY_EX
  (`PeerIdentity.longTermPub`, `System_ESPNow_Identity.h:107`) — AND (b) sending over an
  **authenticated encrypted session** (`ctx.isSessionEncrypted`) AND (c) whose Ed25519
  fingerprint matches the configured master or backup-master fingerprint. MAC alone is
  never trusted, and **paired-plus-session alone is not sufficient** — the fingerprint
  compare is mandatory on *every* control path (§2).
- **Scope:** narrow, non-admin — "control my sensor streaming" only. Never reaches
  `executeCommand`/RCE. New opcode is `REQ_PAIRED|REQ_SESSION_ENC`, **not**
  `REQ_BOND_MODE`. The bond admin channel is untouched.
- **Dual controllers:** worker authorizes TWO Ed25519 fingerprints — PRIMARY and BACKUP
  master. Both are **standing, equal controllers at all times** (there is no worker-side
  binding to actual promotion state), so the promoted backup is already trusted at
  failover — and the backup key must be protected to the same standard as the primary
  (§2, §6).
- **Cadence:** 30s default fetch interval; ~90s lease (3× cadence tolerance).
- **Targeting:** whole mesh — all alive paired peers (`isMeshPeerAlive`,
  `System_ESPNow.cpp:7548`).
- **History:** accepted readings persist to a dedicated history file in addition to the
  live `gRemoteSensorCache` (`System_ESPNow_Sensors.cpp:57`).
- **Opcodes:** `SENSOR_REQ = 153` (control) **and** `SENSOR_ENVELOPE = 154` (the
  **session-encrypted unicast data reply**) — both allocated in phase 0, the
  already-earmarked mesh-legal slots at `System_ESPNow_Wire.h:157`. The plaintext
  `SENSOR_BROADCAST = 150` mesh path (`System_ESPNow_Wire.h:154`) is **removed entirely**
  (decision D2, §2 confidentiality): encrypted unicast `SENSOR_ENVELOPE` is the **only**
  over-the-air sensor-data path, ever. (Bond streaming `BOND_SENSOR_DATA = 179` is already
  encrypted and is unaffected.)
- **Control path (decision D1):** the ONLY remote way to control a worker's streaming is a
  fingerprint-gated `SENSOR_REQ`. The worker **rejects remote invocation of
  `espnowsensorstream`** — that pre-existing command becomes **local-console-only**, so the
  generic credentialed remote-exec route (`espnowremote … espnowsensorstream …`) can no
  longer touch sensor streaming. The fingerprint gate is the *sole* remote authority; this
  eliminates (not merely mitigates) the H1 parallel-path concern.
- **Repo rules:** settings persist via real per-setting commands only (no auto-register);
  reuse existing helpers; no backwards-compat (user erases before flashing).

---

## 2. Threat model & security design

### Trust anchor: securepair, not MAC

Pairing persists each peer's raw Ed25519 public key to
`/system/espnow/peers/<MAC>/identity.json` (`longTermPubEd25519_hex`,
`System_ESPNow_Identity.h:83-88`, written `System_ESPNow_Identity.cpp:391`), cached in
`gPeerIdentities[16]` (`System_ESPNow_Identity.cpp:298`). The lookup
`peerIdentityFindByMac()` (`System_ESPNow_Identity.cpp:463-466`) returns that pubkey by
MAC.

The crypto proof at request time is `ctx.isSessionEncrypted`. It is set true **only**
when a `SESSION_FRAME` AEAD-unwraps successfully (`System_ESPNow.cpp:4738-4777`,
`wasSessionEncrypted=true` at `:4774`). Session keys derive from an X25519 shared secret
whose `SESSION_OPEN/CONFIRM/REKEY` transcripts are Ed25519-signed and verified against the
peer's stored `longTermPub` (`System_ESPNow.cpp:4453-4461`,
`System_ESPNow_Sessions.h:11-17`). Therefore a session-encrypted frame from MAC X
**cryptographically** proves the sender holds X's private key — not merely that it can
put X in the source-MAC field.

### Why MAC-alone is rejected

`ctx.isPaired` is computed by scanning the plaintext `gEspNow->devices[]` peer table for
a MAC match (`System_ESPNow.cpp:5028-5036`) — it is spoofable. The existing failover gate
already learned this lesson: master-liveness is trusted only when
`ctx.isAuthenticated && src==meshMasterMAC` (`System_ESPNow.cpp:2936-2944`), because a
plaintext heartbeat can forge the master MAC. Our command path applies the *stronger*
`REQ_SESSION_ENC` gate plus a fingerprint compare, so MAC spoofing buys an attacker
nothing.

### Single control authority — SENSOR_REQ only (review issue H1, decision D1)

**Accurate statement of the pre-existing surface.** The remote-command handler
`v4_handle_cmd` (`System_ESPNow.cpp:5199`) rejects any command that is not
**session-encrypted** (`:5225`) AND does not carry either a valid **bond token** (`:5256`)
or a valid **`username:password`** that exists in the *worker's own* `users.json`
(`isValidUser`, `:5311`). So a remote peer cannot drive a worker's sensors "just by being
paired" — the earlier "any paired peer" framing was imprecise. The real bar is: *a peer
holding valid credentials for any account on the worker* (and because `espnowsensorstream` is
`requiresAdmin=false` at `System_ESPNow.cpp:14844`, even a **non-admin** account suffices)
could run `espnowsensorstream <sensor> on` on it via the ordinary remote-exec route — the
same route `espnowremote <target> <user> <pass> <cmd>` has always exposed. This is a
pre-existing capability, **not** something this feature introduces.

**Why it still matters.** The new model's whole point is that a worker's streaming is
controlled by *its designated master/backup and no one else*. "Any valid account on the
worker" is a strictly broader authority than "the two configured fingerprints," and the
CMD route also leaves `gSensorLeaseExpiresAt[i] = 0`, so a stream enabled that way would
never auto-stop. Left open, the fingerprint gate would not be the *sole* authority it
claims to be.

**Decision D1 — close it, don't gate it.** The `SENSOR_REQ` opcode is the **only** remote
path to control worker streaming. `cmd_espnow_sensorstream` is made **local-console-only**:
its handler rejects any invocation whose origin is `ORIGIN_ESPNOW` (the origin is already
stamped on the `Command` — `cmd.ctx.origin = ORIGIN_ESPNOW`, `System_ESPNow.cpp:5376`), with
a message pointing to the fingerprint-gated `SENSOR_REQ` path. There is therefore exactly
one authorization predicate on the wire:

```
bool espnowSensorControlAuthorized(const uint8_t mac[6]);  // paired + fingerprint == master|backup
```

called in-handler by `v4h_sensor_req` (§4). No second, weaker route exists — a credentialed
non-admin user, a guest account, even `kBondAdminUser`, can no longer turn a worker's
sensors on remotely, because the *command* that did so is no longer reachable over ESP-NOW.
This is simpler than gating the remote branch (no need to thread the caller MAC into
`executeCommand`) and strictly tighter.

### Narrow, non-admin scope

The `SENSOR_REQ` handler mirrors the existing narrow `v4h_stream_ctrl`
(`System_ESPNow.cpp:3118-3126`): it does a few field writes into `EspNowState` and
returns. It **never** calls `executeCommand`, sets no `AuthContext` user, and cannot
reach RCE. Contrast BOND: `@BOND:<token>:cmd` validates a per-session token
(`validateBondSessionToken`, `System_ESPNow.cpp:5256-5266`) and runs under
`kBondAdminUser` = `bond-admin` (`System_User.h:78`) for which both `isAdminUser`
(`System_User.cpp:290-293`) and `isSuperAdminUser` (`System_User.cpp:363-366`) return
true — the only over-the-air path to super, and it flows into `executeCommand`. Our scope
stays completely disjoint from that channel.

### Dual-fingerprint authorization — and its standing blast radius (review issue L5)

The worker persists two Ed25519 fingerprints: `espnowMasterFingerprint` and
`espnowBackupMasterFingerprint` (§8). `espnowSensorControlAuthorized()` resolves the
sender's identity — `peerIdentityFindByMac(ctx.recv_info->src_addr)` → `longTermPub` → hex
via `espnowIdentityFormatPubHex` (`System_ESPNow_Identity.cpp:275-281`) — and accepts only
if that fingerprint equals the master **OR** the backup fingerprint. Because both are
provisioned up front, the promoted backup is already trusted without any worker
reconfiguration (§6).

**Threat-model completeness:** there is **no worker-side binding to actual promotion
state**. `hb.role` is never consumed on RX (per the failover map), and `gBackupPromoted`
is backup-side only. So the backup fingerprint B is a **standing, always-trusted
controller** identical to the primary P — *at any time*, not only during a genuine
failover. Consequently, **provisioning a backup doubles the set of always-trusted
controllers**, and B's key must be protected to the same standard as P's; compromise of
either key yields full sensor-control authority over every worker. Operators should size
the backup-key risk accordingly.

### Confidentiality of leased data (review issue H2)

The first draft reused the plaintext `SENSOR_BROADCAST = 150` reply. That path
(`v4_broadcast_sensor_data` → `v4_broadcast_category`, `System_ESPNow.cpp:1882-1935`) sends
per-peer **plaintext + `BROADCAST_AUTH` HMAC** — *authenticity only, not encryption*
(comment `System_ESPNow.cpp:1521`) — to **every** active mesh peer where
`peerIdentityWantsEvent(mac, ESPNOW_EVT_SENSOR)` is true (default
`subscribedEvents = ESPNOW_EVT_ALL`, `System_ESPNow_Identity.cpp:451/489`). Two problems the
draft missed:

1. **Confidentiality:** leased readings would be broadcast in cleartext — every
   default-subscribed paired peer *and any passive RF sniffer* (no pairing needed) could
   read them. The control channel would protect who can turn streaming *on* but not who can
   *read* the data.
2. **Delivery/failover:** delivery would depend on the recipient being an active mesh peer
   *with the `SENSOR` subscription bit* — a property entirely decoupled from the new
   fingerprint authorization. A fingerprint-authorized controller (especially a promoted
   backup B that may not be a subscribed mesh peer of every worker) could succeed at
   *control* yet receive **no data**, while §5's "alive but no readings → re-subscribe"
   loop spins forever and some *other* subscribed peer harvests the stream.

**Fix (folded into §3/§4/§5):** the leased data reply is a **session-encrypted unicast**
addressed to the leasing master's MAC, sent as `SENSOR_ENVELOPE = 154` via
`v4_send_payload_smart`/`sendAead` (encrypt-or-fail), targeting the fingerprint-authorized
requester **recorded at lease time**. This fixes confidentiality (AEAD, not HMAC-over-
plaintext), targeted delivery (unicast to the authorized controller, not "whoever
subscribed"), and failover (a promoted B that leases a worker automatically becomes that
worker's data target) in one move.

**Decision D2 — remove the plaintext path entirely.** Rather than keep
`SENSOR_BROADCAST = 150` alive for "local/manual" streaming (a half-measure — it would still
put GPS/telemetry on-air in cleartext whenever someone toggled it), the plaintext mesh
sensor-data path is **deleted**. Encrypted unicast `SENSOR_ENVELOPE` is the *only*
over-the-air sensor-data path. `transmitSensorData`'s mesh branch
(`v4_broadcast_sensor_data` → `v4_broadcast_category`, `System_ESPNow.cpp:1882-1935`) is
removed; the manual `espnowsensorstream` local toggle now emits data **only** to the
currently authorized controller (`gSensorControllerMac`, set at lease grant) via encrypted
unicast, and with no active controller it is a no-op on the air (local flag only). There is
no circumstance under which sensor data leaves a worker in plaintext. (Bond streaming
`BOND_SENSOR_DATA = 179` was already encrypted and is untouched.)

### Dispatch-flag registration

Both new rows (`SENSOR_REQ`, `SENSOR_ENVELOPE`) are registered
`REQ_PAIRED | REQ_SESSION_ENC`, exactly the pattern the `FS_*` and `FILE_*` rows already
use (`System_ESPNow.cpp:4490-4509`) — no `REQ_BOND_MODE` bit, so `gSettings.bondModeEnabled`
is never consulted (`:4564` only fires when the bond bit is set). Enforcement is central in
`v4_dispatch_table_try`: `REQ_PAIRED` drops non-paired (`:4558`), `REQ_SESSION_ENC` drops
non-session-encrypted (`:4584`), and only then is the handler invoked (`:4605`). Flags are
defined at `System_ESPNow.cpp:2691-2704`. Registering `SENSOR_ENVELOPE` with
`REQ_SESSION_ENC` means the **master's** ingest of leased data is itself AEAD-protected.

### Invariants preserved

- **Plaintext-CMD rejection** (`System_ESPNow.cpp:5222-5240`) is untouched — we add new
  opcodes, we do not relax `v4h_cmd`. Decision D1 *removes* remote reachability of
  `espnowsensorstream` (making it local-console-only), which strictly tightens the CMD
  surface; it does not loosen the invariant.
- **Bond channel untouched** — no `REQ_BOND_MODE`, no `kBondAdminUser`, no
  `bondDeferredStreamCtrl*` reuse for the mesh path (the mesh path gets its own deferred
  fields, §4).
- **Silent-drop for unregistered opcodes** (`System_ESPNow.cpp:4526-4535`) means each
  opcode, dispatch row, payload struct, and handler must land together (§3 charter steps).

### What an attacker can and cannot do

| Attacker capability | Outcome |
|---|---|
| Spoof the master's MAC in a plaintext frame | Dropped at `REQ_SESSION_ENC` (`:4584`); no session, no unwrap. |
| Replay a captured `SENSOR_REQ` | Dropped — AEAD/session nonce state rejects replays at unwrap (`:4738-4777`); at worst re-arms an already-authorized lease. |
| A *paired* but wrong-fingerprint peer sends `SENSOR_REQ` over a valid session | Passes dispatch flags, fails `espnowSensorControlAuthorized()` (≠ master and ≠ backup) → ignored. |
| A credentialed (even non-admin) user invokes `espnowsensorstream on` remotely via `espnowremote` (the old broader route) | Rejected — `cmd_espnow_sensorstream` refuses `ORIGIN_ESPNOW` invocations (D1); the only remote control path is fingerprint-gated `SENSOR_REQ`. |
| Passive RF sniffer (no pairing) or any other peer tries to read sensor data | Cannot — sensor data is **only ever** AEAD session-encrypted unicast (`SENSOR_ENVELOPE`); the plaintext `SENSOR_BROADCAST` path no longer exists (D2). |
| Compromise a worker | Bounded to sensor streaming; no RCE, no admin, no bond token — the scope never touches `executeCommand`. |
| Compromise **either** provisioned key (primary *or* backup) | Full standing sensor-control authority over every worker — both fingerprints are equal standing controllers (L5); protect both keys equally. |
| Master goes down / never comes up / ESP-NOW not set up | Worker never streams into the void; lease lapses and streaming auto-stops (§4). Worker never auto-streams on boot. |

An attacker who holds a trusted master's Ed25519 private key can, of course, control that
worker's sensor streaming and receive its (encrypted) data — that is the intended
authority and the whole point of pairing.

---

## 3. Wire protocol

### Opcode allocation (one-commit charter)

`System_ESPNow_Wire.h:157` currently reads `// 153/154 earmarked SENSOR_REQ /
SENSOR_ENVELOPE (mesh-legal on-demand pull)` — comment only, no symbol/struct/handler.
Per the Wire.h charter, allocate **both** opcodes in ONE commit (the H2 fix promotes
`SENSOR_ENVELOPE` from "reserved, maybe later" to a phase-0 allocation):

1. **Enum symbols** — replace the earmark comment with
   `ESPNOW_V4_TYPE_SENSOR_REQ = 153,` and `ESPNOW_V4_TYPE_SENSOR_ENVELOPE = 154,`.
   Slots 155–169 remain free for the Sensors family.
2. **Payload structs** — `V4PayloadSensorReq` (below) and the envelope reply payload in
   `System_ESPNow_Wire.h`, each with a
   `static_assert(sizeof(...) <= ESPNOW_V4_MAX_PLAINTEXT, ...)` mirroring every other
   payload (e.g. `System_ESPNow_Wire.h:340`).
3. **Dispatch rows** —
   `{ ESPNOW_V4_TYPE_SENSOR_REQ, V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC, v4h_sensor_req }`
   and
   `{ ESPNOW_V4_TYPE_SENSOR_ENVELOPE, V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC, v4h_sensor_envelope }`
   in the V4 dispatch table (`System_ESPNow.cpp:4478-4517`), placed near the `FS_*` rows
   they mirror.
4. **Handlers** — `v4h_sensor_req` (§4, worker RX) and `v4h_sensor_envelope` (§5, master
   RX / ingest).

### SENSOR_REQ (153) payload

The frame header already carries `uint32_t msgId` at wire offset 8–11
(`System_ESPNow_Wire.h:207`); use it for request→reply correlation, so no reqId is needed
in the body — but include a small app-level `reqId` too for the master's own renewal
bookkeeping and logging.

```
struct V4PayloadSensorReq {
  uint8_t  reqId;         // app-level correlation / renewal generation
  uint8_t  mode;          // 0=subscribe, 1=unsubscribe, 2=oneshot
  uint32_t sensorMask;    // bitmask over RemoteSensorType (U32 headroom; see §10)
  uint16_t intervalMs;    // desired per-sensor cadence hint (worker still floors at spec.minIntervalMs)
  uint16_t leaseMs;       // lease duration; 0 with mode=subscribe is invalid
};                        // 10 bytes — trivially within 202-byte plaintext budget
```

- `sensorMask` is preferred over a single `sensorType` so one frame leases the whole
  set (thermal/camera/mic/apds bits are simply ignored by workers with null builders,
  `System_ESPNow_Sensors.cpp:101`). A single-type request is `sensorMask` with one bit
  set.
- `mode=oneshot` requests a single immediate push then no lease — useful for
  on-demand refresh and for the manual test CLI (§9).

### SENSOR_ENVELOPE (154) — the data reply (H2 fix)

The leased data reply is a **session-encrypted unicast** to the leasing master, NOT the
plaintext `SENSOR_BROADCAST` broadcast. `SENSOR_ENVELOPE` wraps the **identical**
per-sensor serialized reading that the existing `transmitSensorData` path already produces
and that `v4h_sensor_broadcast` already knows how to parse (same size discipline — the
existing per-sensor 200-byte frame cap and null-builder rules apply unchanged,
`System_ESPNow_Sensors.cpp:101`). The only differences are addressing and crypto:

- **Addressed** to the fingerprint-authorized requester's MAC recorded at lease grant
  (`gSensorControllerMac`, §4), not broadcast to all subscribers.
- **Sent** via the existing encrypt-or-fail send path `v4_send_payload_smart`/`sendAead`
  (the same machinery bond and FS use), so it is AEAD-confidential, not HMAC-over-
  plaintext.
- **Ingested** by the master's `v4h_sensor_envelope` handler into `gRemoteSensorCache`
  reusing the `findOrCreateCacheEntry` cache-update logic the old `v4h_sensor_broadcast`
  contained (`System_ESPNow_Sensors.cpp:238-264`) — see §5/§7. With `SENSOR_BROADCAST`
  removed (D2), `v4h_sensor_envelope` is the single ingest front door.

The plaintext `SENSOR_BROADCAST = 150` transmit path is **removed** (decision D2): no
sensor data ever leaves a worker in cleartext. The per-sensor serialization/parse logic that
`v4h_sensor_broadcast` used is reused by the new `v4h_sensor_envelope` ingest handler (§5) —
only the transport (encrypted unicast vs plaintext broadcast) changes. Opcode 150 itself may
be retired or left reserved; either way nothing transmits it.

There is no separate subscribe-ACK opcode: the arrival of `SENSOR_ENVELOPE` frames from a
worker within the grace window *is* the acknowledgement the master needs (wire-delivery ≠
command-success on this mesh, so an explicit ACK would be equally unreliable — §5 treats
"no readings within grace" as re-subscribe).

### Size vs `ESPNOW_V4_MAX_PLAINTEXT`

`ESPNOW_V4_MAX_PLAINTEXT` = 202 bytes (`System_ESPNow_Wire.h:40`). The 10-byte request is
far within budget; the envelope inherits the same per-sensor size discipline the existing
broadcast already enforces. The `static_assert`s in step 2 enforce the fixed-size bounds at
compile time.

---

## 4. Worker side

### New settings (worker role)

Three persisted fields (full registration in §8):

- `String espnowMasterFingerprint` — primary controller Ed25519 fingerprint (64-hex).
- `String espnowBackupMasterFingerprint` — backup controller fingerprint (64-hex).
- `bool espnowAcceptSensorControl` — master enable/disable of the whole feature on this
  worker (defaults **false**: opt-in, so a freshly flashed worker never accepts control
  until provisioned).

Both fingerprints are PUBLIC identifiers → `isSecret=false`, mirroring `meshMasterMAC`
(`System_ESPNow.cpp:14934`), not the `bleSecureChannelSecret`/`mqttPassword` secret
pattern (`Bluetooth.cpp:2121`, `System_MQTT.cpp:212`).

### Shared authorization helper (single source of truth — H1)

```
bool espnowSensorControlAuthorized(const uint8_t mac[6]);
```

- Resolve `peerIdentityFindByMac(mac)` (`System_ESPNow_Identity.cpp:463`); if null → deny.
- Format `id->longTermPub` via `espnowIdentityFormatPubHex`
  (`System_ESPNow_Identity.cpp:275`) **or** keep the raw 32 bytes.
- Compare against `espnowMasterFingerprint` **and** `espnowBackupMasterFingerprint`.
  **Empty / malformed configured fingerprints must never match (review issue L4):** a
  configured value that is not exactly 64 hex chars (or does not decode to exactly 32
  bytes) is treated as "deny — skip this comparand," **not** compared. Prefer decoding both
  the configured value and the live pubkey to exactly 32 bytes and `memcmp`-ing 32 bytes;
  a raw memcmp of a zero-length decoded `""` against 0 bytes would spuriously succeed, so
  guard the length first. If neither master nor backup yields a valid 32-byte comparand,
  deny.

This helper is the **sole** on-wire authorization for sensor control: `v4h_sensor_req`
calls it, and no other remote path exists (`cmd_espnow_sensorstream` is local-console-only
per D1). There is no second, weaker authorization path.

### RX handler `v4h_sensor_req` (defer to super-loop)

Mirror `v4h_stream_ctrl` exactly (`System_ESPNow.cpp:3118-3126`): run cheap, inline on
`espnow_task`, do no JSON/FS/task work. Steps:

1. **Feature gate:** if `!gSettings.espnowAcceptSensorControl`, drop.
2. **Authorization (the crux):** `espnowSensorControlAuthorized(ctx.recv_info->src_addr)`;
   if it returns false, drop. The dispatch flags (`REQ_PAIRED | REQ_SESSION_ENC`) have
   already guaranteed paired + session-encrypted before we reach here; the helper adds the
   mandatory fingerprint match and the L4 empty-deny guard.
3. **Defer:** stash into new `EspNowState` fields (parallel to `bondDeferredStreamCtrl*`,
   `System_ESPNow.h:730-732`, but physically separate from them), e.g.
   `meshSensorReqMask`, `meshSensorReqMode`, `meshSensorReqIntervalMs`,
   `meshSensorReqLeaseMs`, `meshSensorReqSrcMac[6]` (the authorized controller's MAC, for
   the encrypted-unicast reply target), `meshSensorReqPending = true`.
   Do NOT reuse the bond `bondDeferredStreamCtrl*` fields — keep the mesh path physically
   separate from bond.

### Super-loop apply

In `processMeshHeartbeats` (`espnow_task`, ~100Hz via `espnowHeartbeatTaskFn`,
`System_ESPNow.cpp:8738-8743`), add a block that consumes `meshSensorReqPending` and, per
mode:

- `subscribe`: record `gSensorControllerMac = meshSensorReqSrcMac` (the encrypted-reply
  target). For each bit set in `meshSensorReqMask`, call
  `startSensorDataStreaming((RemoteSensorType)i)` (`System_ESPNow_Sensors.cpp:331`) — the
  same entry the human `cmd_espnow_sensorstream` uses (`:889-946`) — and stamp
  `gSensorLeaseExpiresAt[i] = millis() + leaseMs`.
- `unsubscribe`: for each masked bit, clear the lease and call
  `stopSensorDataStreaming((RemoteSensorType)i)` (`:390`). This call is safe here because
  the super-loop runs on `espnow_task`, **not** the broadcaster task — deleting a
  *different* task's handle returns normally (contrast the in-task expiry path below).
- `oneshot`: `startSensorDataStreaming` with a very short lease (e.g. one interval) so it
  self-stops after a single push.

This apply runs off the RX critical path, exactly as the BOND deferred apply does
(`System_ESPNow.cpp:8536-8552`).

### Lease bookkeeping in `sensorBroadcasterTask` (self-teardown, not self-delete — H3)

Add a parallel array `static uint32_t gSensorLeaseExpiresAt[REMOTE_SENSOR_MAX] = {0};`
next to `gSensorStreamingEnabled[]` (`System_ESPNow_Sensors.cpp:64`) and `gLastTxMs[]`
(`:72`), plus a module-level `gSensorControllerMac[6]` (the current encrypted-reply
target).

**Critical correctness fix (H3):** the lease-expiry branch runs *inside*
`sensorBroadcasterTask`. It must **not** call `stopSensorDataStreaming` /
`stopSensorBroadcaster`, because when the expiring lease is the last enabled sensor (the
common single-sensor case) `stopSensorDataStreaming` detects all-disabled and calls
`stopSensorBroadcaster()`, which does `vTaskDelete(gSensorBroadcasterTask)`
(`System_ESPNow_Sensors.cpp:680-686`) — and that handle **is** the currently running task.
A task deleting itself via its own handle does not return from `vTaskDelete`, so the very
next line `gSensorBroadcasterTask = nullptr` (`:685`) never executes; the global keeps a
freed/stale handle, `startSensorBroadcaster()` then early-returns true (`:642`), and the
worker can **never stream again until reboot**. The existing code is safe only because
`stopSensorDataStreaming` is presently invoked from *other* task contexts; this design is
what newly introduces the self-delete, so it must avoid it.

In the per-sensor loop (`:611-634`), immediately after the
`if (!gSensorStreamingEnabled[i]) continue;` gate at `:611-612`, insert **only a flag
clear** — no stop call:

```
if (gSensorLeaseExpiresAt[i] != 0 && (uint32_t)now >= gSensorLeaseExpiresAt[i]) {
    gSensorLeaseExpiresAt[i] = 0;
    gSensorStreamingEnabled[i] = false;
    broadcastSensorStatus(i, false);   // optional: notify status
    continue;
}
```

Then, **after** the per-sensor loop, if no sensors remain enabled, perform a clean
in-task self-teardown:

```
// still inside the for(;;) body, after the per-sensor loop:
if (noSensorsEnabled()) {
    gSensorBroadcasterTask = nullptr;   // NULL the handle FIRST
    break;                              // leave the for(;;)
}
// ... fall through to vTaskDelete(NULL) at the end of the task function.
```

The task deletes **itself with `NULL`** (which never needs to return) and the global handle
was already cleared, so `startSensorDataStreaming` (`:364`) / `startSensorBroadcaster`
(`:642`) can cleanly recreate the task on the next lease. A lease value of `0` means "no
lease / never auto-expire" — used by the local-console manual toggle (whose data still only
leaves the worker encrypted-unicast to the current controller, §2 D2).

### Reply emission — one path, always encrypted unicast (H2 / D2)

Every sensor reading the broadcaster sends — leased or manual — is emitted as
`SENSOR_ENVELOPE = 154` to `gSensorControllerMac` via `v4_send_payload_smart`/`sendAead`
(encrypt-or-fail), wrapping the serialized reading `transmitSensorData` builds. There is no
plaintext branch: the `v4_broadcast_sensor_data` mesh path is deleted (D2). If `sendAead`
cannot encrypt (no live session to the controller) **or** no controller MAC is set, the
frame is simply not sent — never a plaintext fallback. Concretely:

- **Leased sensor** (`gSensorLeaseExpiresAt[i] != 0`): encrypted unicast to the lease-time
  controller.
- **Manual/local sensor** (`lease == 0`, enabled from the local console): encrypted unicast
  to `gSensorControllerMac` **if a controller is currently set** (i.e. the master/backup has
  an active or recent lease on this worker); otherwise the local flag is set but nothing
  goes on the air. Manual toggle never broadcasts.

### `cmd_espnow_sensorstream` — local-console-only (close the parallel path, D1)

`cmd_espnow_sensorstream` (`System_ESPNow_Sensors.cpp:889-946`) is made
**local-console-only**. At the top of the handler, reject any `ORIGIN_ESPNOW` invocation
(the origin is on `cmd.ctx.origin`, stamped `ORIGIN_ESPNOW` at `System_ESPNow.cpp:5376`)
with a message directing the caller to the fingerprint-gated `SENSOR_REQ` path. Local
invocations keep the leaseless (`lease = 0`) flag-toggle behavior — but per the reply rule
above, data only reaches the wire, encrypted, when a controller is set. This removes the
generic credentialed remote-exec route to sensor control entirely (D1); there is no remote
branch to authorize.

### Boot behavior

`gSensorBroadcastEnabled` defaults false (`System_ESPNow_Sensors.cpp:61`) and nothing
enables it at boot; `espnowAcceptSensorControl` defaults false. A worker therefore never
auto-streams: it streams only after a trusted, session-encrypted, fingerprint-matched
request arrives, and always self-stops when the lease lapses. If the master is down, never
comes up, or ESP-NOW is not configured, the worker stays silent — no streaming into the
void.

---

## 5. Master side

### Role gate

Fetch-tick activation must fire when this node is the configured master **or** a
runtime-promoted backup. `gSettings.meshRole` persists 0/1/2 (`System_Settings.h:676-679`;
enum `System_ESPNow.h:9-13`); failover flips role at runtime only via `setMeshRole`
(`System_ESPNow.cpp:7661-7667`, no persist). So the master tick gates on the **runtime**
role via the shared `meshActingAsMaster()` predicate (§6).

### New settings (master role)

Persisted (§8): `bool espnowSensorFetchEnable` (feature master switch),
`uint32_t espnowSensorFetchIntervalMs` (default 30000),
`uint32_t espnowSensorFetchMask` (which sensors to lease mesh-wide),
`bool espnowSensorFetchHistoryEnable` (§7). Interval/mask use U32 for headroom.

### `v4h_sensor_envelope` — ingest leased data (H2)

The master registers `v4h_sensor_envelope` (opcode 154, `REQ_PAIRED | REQ_SESSION_ENC`). It
AEAD-unwraps (guaranteed by the dispatch flag), parses the wrapped per-sensor reading, and
updates `gRemoteSensorCache` via the `findOrCreateCacheEntry` cache-update helper
(`System_ESPNow_Sensors.cpp:238-264`, formerly used by the now-removed `v4h_sensor_broadcast`)
— the **single** ingest path (D2). It also stamps `MeshPeerMeta.lastReadingMs` for the
source peer (below).
Because the frame is unicast+encrypted, only the addressed, fingerprint-authorized master
receives it, and a promoted backup that leased the worker automatically becomes the
recipient — control and delivery can no longer diverge.

### Periodic 30s tick in `processMeshHeartbeats`

Model on the BOND SYNC TICK (`System_ESPNow.cpp:8287-8340`) — a master-driven,
idempotent "request what's needed" block inside the already-100Hz super-loop, throttled
with `everyMs(&lastFetchTickMs, gSettings.espnowSensorFetchIntervalMs)`
(`System_Utils.h:205`), the same primitive the bond cooldown and `systemEventLogTick` use.
Guard the whole block on `espnowSensorFetchEnable && meshActingAsMaster()`.

Per tick, iterate the mesh peer table `gMeshPeers[0..gMeshPeerSlots)`
(`System_ESPNow.h:1097`, `:179`) and, for each peer passing `isMeshPeerAlive(&p)`
(`System_ESPNow.cpp:7548`, requires `isActive` + heartbeat within
`MESH_PEER_TIMEOUT_MS=30000`, `System_ESPNow.h:176`), send/renew a `SENSOR_REQ`
(`mode=subscribe`, `sensorMask=espnowSensorFetchMask`, `leaseMs≈90000`,
`intervalMs=espnowSensorFetchIntervalMs`) over an encrypted session via the existing
encrypt-or-queue send path (the same `v4_send_payload_smart`/`sendAead` machinery bond and
FS use). Using the stricter `isMeshPeerAlive` (not just `isActive`) avoids re-leasing
powered-off peers, matching the online/offline sweep (`System_ESPNow.cpp:8168-8173`).

Because a 90s lease is 3× the 30s renewal cadence, up to two consecutive missed renewals
are tolerated before a worker self-stops — smoothing transient loss without ever streaming
into a dead master.

### Subscription / renewal bookkeeping

Master-side state is RAM-only (leases are ephemeral). The cleanest home is
`MeshPeerMeta`, which is already per-MAC and already carries a `sensorMask`
(`System_ESPNow.h:217-227`, mask at `:225`); add a `uint32_t sensorLeaseSentMs` (last
renewal) and `uint32_t lastReadingMs` (last `SENSOR_ENVELOPE` ingested for this peer,
stamped by `v4h_sensor_envelope`). Do NOT hang lease state on
`PeerIdentity.subscribedEvents` — that is the wrong direction (events peers want *from*
us) and persists to disk immediately (`System_ESPNow_Identity.cpp:145`), unsuitable for a
fast-cycling RAM lease.

### Re-subscribe after a worker reboot

A rebooted worker comes back with `espnowAcceptSensorControl` true (persisted) but no
active streaming and no live lease. The master's next 30s tick simply re-sends
`SENSOR_REQ` (the tick is idempotent — it always renews all alive peers), so the worker
re-arms automatically within one cadence. Additionally, since **wire-delivery ≠
command-success** on this mesh, the master treats a peer that is alive but has produced no
`SENSOR_ENVELOPE` within a grace window (e.g. `2 × intervalMs`) as needing re-subscribe
*now* rather than waiting for the next tick — using `lastReadingMs` in `MeshPeerMeta`.
Because delivery is now the encrypted unicast addressed to *this* master, `lastReadingMs`
is a faithful signal that *this* controller is actually receiving data (it can no longer be
satisfied by some other subscribed peer harvesting a broadcast, the H2 failure mode).

---

## 6. Failover behavior

Setup: PRIMARY master P and BACKUP master B are both provisioned on every worker as
`espnowMasterFingerprint = fp(P)` and `espnowBackupMasterFingerprint = fp(B)`. B is
designated on P via `espnow meshbackup <MAC>` + `espnow backupenable`
(`System_ESPNow.cpp:11328-11381`); B tracks P's heartbeats (`gLastMasterHeartbeat`,
anti-spoof gated at `:2936-2944`).

**Standing-authority note (L5):** because the worker has no wire-derived binding to actual
promotion state, B is a trusted controller *at all times*, not only after promotion. This
is what makes failover seamless — and also why B's key carries the same blast radius as
P's (§2). Provisioning a backup is a deliberate doubling of the always-trusted controller
set.

When P dies:

1. B's heartbeat task detects silence ≥ `meshFailoverTimeout`, sets `gBackupPromoted=true`
   and `setMeshRole(MASTER, "backup.promoted")` at runtime (`System_ESPNow.cpp:8061-8075`,
   posts `SYSEVT_MESH_PROMOTED`).
2. B's `processMeshHeartbeats` now passes `meshActingAsMaster()` and **starts its own fetch
   tick** (§5) against all alive paired peers.
3. Workers receive `SENSOR_REQ` from B over B's session. `espnowSensorControlAuthorized()`
   matches `espnowBackupMasterFingerprint` → **accepted with no worker reconfiguration** —
   and each worker records B's MAC as `gSensorControllerMac`, so its encrypted
   `SENSOR_ENVELOPE` replies now flow **to B**. This is why the H2 unicast-to-requester
   design makes failover data delivery automatic: control and delivery move together.
4. Any leases granted under P lapse naturally (~90s); B's renewals keep streaming
   continuous. Worst case a worker sees at most ~90s of silence before B's first renewal,
   which is within lease tolerance.

When P returns: B auto-demotes (`setMeshRole(BACKUP_MASTER, "backup.master_returned")`,
`System_ESPNow.cpp:2941-2953`, posts `SYSEVT_MESH_DEMOTED`), its fetch tick gate goes
false, and P's tick resumes — workers re-record P's MAC as `gSensorControllerMac` on P's
next renewal and replies flow back to P. Because both fingerprints stay provisioned, this
hand-back is transparent to workers.

### `meshActingAsMaster()` — one predicate, no drift

Two role checks must agree on whether a promoted backup counts as "master":

- The worker stream-control command handler currently rejects only when local role
  `==MASTER` (`System_ESPNow_Sensors.cpp:927`) — i.e., masters receive, everyone else may
  stream.
- The new master fetch-tick gate must fire for a promoted backup.

`setMeshRole` sets `meshRole = MESH_ROLE_MASTER` on promotion (`:7661-7667`), so
`== MESH_ROLE_MASTER` already captures a promoted backup at runtime. **Resolution:** add one
shared inline predicate `bool meshActingAsMaster()` returning
`gSettings.meshRole == MESH_ROLE_MASTER` (true for a promoted backup, because promotion
overwrites the runtime role) and use it for **both** the fetch-tick gate and the worker's
"am I a receiver, don't stream" check, so the two paths can never drift. Document that
`MESH_ROLE_BACKUP_MASTER` means "backup, not yet promoted" and that promotion is expressed
by the role *becoming* `MASTER` at runtime, not by a separate flag.

---

## 7. Data surfacing & history

### Live surfacing (reuse, no new plumbing)

Accepted readings flow into `gRemoteSensorCache` via a **single** ingest handler,
`v4h_sensor_envelope` (the encrypted-unicast ingest, §5), reusing the `findOrCreateCacheEntry`
cache-update helper (receive-side per device+sensor, TTL'd, `System_ESPNow_Sensors.cpp:57`,
`:238-264`). With the plaintext `SENSOR_BROADCAST` path removed (D2), there is no second
ingest route.
Everything downstream of that cache works unchanged: `/api/sensors/remote`,
`espnowsensorstatus`, and the OLED/G2 remote-sensor views. No changes needed — the fetcher
just *drives* the cache to stay populated.

### History file sink

Gated on `espnowSensorFetchHistoryEnable`. Append each accepted remote reading to
`/system/sys_logs/remote_sensors.log` using `appendLineWithCap(path, line, capBytes)`
(`System_Filesystem.cpp:1812-1871`), which appends and, when over cap, trims the tail past
the first newline into a `.tmp` to keep ~85% — rotation for free, zero new rotation code.
Reuse `LOG_EVENT_STREAM_CAP`-style sizing (256KB, `System_Logging.h:58`) or a dedicated
cap constant (§10).

**Critical placement:** the append MUST run **off** the RX handler — `v4h_sensor_envelope`
runs inline on `espnow_task` (as all V4 handlers do), and taking `fsLock` there would
stall the RX drainer. Add the append to a **master logging tick** that drains
`gRemoteSensorCache` entries whose `lastUpdate` advanced since last drain, and writes one
compact line per new reading (timestamp, source MAC/name, sensor type, value summary). This
mirrors how `sensorLogTick` (`System_SensorLogging.cpp:73`, `openGuarded "a"` under `fsLock`
at `:495-504`) writes local sensor history off-path. Do NOT reuse the `events.log` SYSEVT
pipeline for full readings — its `DETAIL_LEN=80` (`System_Events.h:295-301`) truncates a
full sensor JSON (GPS ~256B, TOF ~1KB); a compact one-line-per-reading `appendLineWithCap`
file is the right fidelity/cost trade.

---

## 8. Settings & command surface

All fields follow the canonical persisted-per-setting-command idiom: typed `gSettings`
field + `SettingEntry` row in `espnowSettingEntries[]` (`System_ESPNow.cpp:14916`) with an
explicit width tag + a real `CommandEntry` row in `espNowCommands[]`
(`System_ESPNow.cpp:14769`). Persistence is `/system/settings.json` via
`writeSettingsJson()` inside `handleSettingCommand`/`setSetting`
(`System_Settings.cpp:2596/2619/2626`) — never raw NVS, never auto-register. Group column
`"mesh"` (or a new `"securefetch"` label) sub-nests them under `network.espnow`.

**Width tags matter** — using untyped `SETTING_INT` on a sub-4-byte field corrupts
adjacent struct members (the 2026-05-18 bug, `System_ESPNow.cpp:14929-14932`). Match the
field width exactly.

### gSettings fields + constructor defaults

Add near `meshMasterMAC`/`sensorBroadcastIntervalMs` (`System_Settings.h:200-224`,
`676-679`):

| Field | Type | Default | Role |
|---|---|---|---|
| `espnowMasterFingerprint` | `String` | `""` | worker |
| `espnowBackupMasterFingerprint` | `String` | `""` | worker |
| `espnowAcceptSensorControl` | `bool` | `false` | worker |
| `espnowSensorFetchEnable` | `bool` | `false` | master |
| `espnowSensorFetchIntervalMs` | `uint32_t` | `30000` | master |
| `espnowSensorFetchMask` | `uint32_t` | (all real sensors) | master |
| `espnowSensorFetchHistoryEnable` | `bool` | `false` | master |

Both fingerprint defaults are `""`, which `espnowSensorControlAuthorized()` treats as
"deny" (L4) — so a worker with `espnowAcceptSensorControl=true` but unprovisioned
fingerprints accepts control from **no one**, rather than accidentally matching a
zero-length comparand.

### SettingEntry rows (copy templates)

- Fingerprints → copy the `masterMAC` string row (`System_ESPNow.cpp:14934`),
  `SETTING_STRING`, `isSecret=false`.
- `acceptSensorControl`, `sensorFetchEnable`, `sensorFetchHistoryEnable` → copy a bool row
  (`meshHeartbeatBroadcast`, `:14942`), `SETTING_BOOL`.
- `sensorFetchIntervalMs` → copy `sensorBroadcastInterval` (`:14946`), but `SETTING_U32`
  with min/max (e.g. 5000–600000).
- `sensorFetchMask` → `SETTING_U32`, with an options string in the bitmask-renderer format
  `"bitmask:1|Thermal,2|ToF,..."` (`System_Settings.h:1194`) so the web/OLED editor draws
  a checkbox grid.

### Command rows and naming (existing espnow convention)

Pure persist-only fields use the `ESPNOW_SETTING_CMD(func, jsonKey)` macro
(`System_ESPNow.cpp:14971`, e.g. `cmd_espnow_sensorbroadcastinterval` `:14986`):

- `espnowmasterfingerprint` (jsonKey `masterFingerprint`)
- `espnowbackupfingerprint` (jsonKey `backupMasterFingerprint`)
- `espnowacceptsensorcontrol` (jsonKey `acceptSensorControl`)
- `espnowsensorfetchmask` (jsonKey `sensorFetchMask`)
- `espnowsensorfetchhistory` (jsonKey `sensorFetchHistoryEnable`)

Live-apply fields use a hand-written command calling `setSetting(...)` (mirrors
`cmd_espnow_meshrole` `:11179-11194`) so the running fetch tick re-reads immediately:

- `espnowsensorfetch` (on|off — `sensorFetchEnable`, mirrors `espnowsensorbroadcast`
  `:14846`)
- `espnowsensorfetchinterval` (`sensorFetchIntervalMs`)

**Admin gating:** all rows `requiresAdmin=true` (3rd `CommandEntry` arg), enforced
centrally in `authorizeCommand`/`commandRequiresAdmin`
(`System_Utils.cpp:4233`, `:3233`). Ordinary admin, NOT super — only identity-wipe ops
(`espnowregenidentity`, `System_ESPNow.cpp:14782`) are super-admin; config toggles match
the `meshRole`/`masterMAC` admin tier.

**Note on `espnowsensorstream`:** this pre-existing command stays `requiresAdmin=false` but
becomes **local-console-only** (D1) — its handler rejects any `ORIGIN_ESPNOW` invocation
(`cmd.ctx.origin`, stamped at `System_ESPNow.cpp:5376`). The only remote way to control a
worker's streaming is the fingerprint-gated `SENSOR_REQ`; there is no remote branch on this
command to authorize. `requiresAdmin` is irrelevant to the trust decision here — the anchor
is physical/local presence for the console path and the Ed25519 fingerprint for the wire
path.

---

## 9. Phased plan

Each phase is independently HW-testable and named to the exact touch points, in
dependency order.

### Phase 0 — Wire allocation (one commit, no behavior)

- `System_ESPNow_Wire.h:157`: allocate **both** `ESPNOW_V4_TYPE_SENSOR_REQ = 153` and
  `ESPNOW_V4_TYPE_SENSOR_ENVELOPE = 154` + structs `V4PayloadSensorReq` and the envelope
  payload + `static_assert`s.
- `System_ESPNow.cpp:4478-4517`: add dispatch rows
  `{ SENSOR_REQ, REQ_PAIRED|REQ_SESSION_ENC, v4h_sensor_req }` and
  `{ SENSOR_ENVELOPE, REQ_PAIRED|REQ_SESSION_ENC, v4h_sensor_envelope }`.
- Stub `v4h_sensor_req` and `v4h_sensor_envelope` that only log (drop everything).
  **Test:** builds green on the primary FeatherS3 board; existing traffic unaffected.

### Phase 1 — Worker settings + authorization + lease bookkeeping + encrypted reply

- Add the three worker `gSettings` fields + SettingEntry rows + commands (§8).
- Add `espnowSensorControlAuthorized()` shared helper with the L4 empty/malformed-deny
  guard (§4).
- Add `gSensorLeaseExpiresAt[REMOTE_SENSOR_MAX]` and `gSensorControllerMac[6]`; implement
  the **in-task flag-clear + clean self-teardown** in `sensorBroadcasterTask`
  (`System_ESPNow_Sensors.cpp:611`) — **no self-`stopSensorBroadcaster`** (H3).
- Flesh out `v4h_sensor_req`: feature gate → `espnowSensorControlAuthorized()` →
  defer via new `EspNowState` fields (including `meshSensorReqSrcMac`).
- Add the super-loop apply block in `processMeshHeartbeats` calling
  `startSensorDataStreaming`/`stopSensorDataStreaming`, stamping the lease and
  `gSensorControllerMac`.
- Implement the **single** `SENSOR_ENVELOPE` encrypted-unicast emission path in the
  broadcaster (all readings → `sendAead` to `gSensorControllerMac`; no controller / no
  session → not sent). **Delete** the plaintext `v4_broadcast_sensor_data` mesh TX path
  (D2/H2).
- Make `cmd_espnow_sensorstream` **local-console-only**: reject `ORIGIN_ESPNOW` at the top
  of the handler (D1/H1). No remote branch.
- Flesh out `v4h_sensor_envelope` ingest into `gRemoteSensorCache` (shared cache helper).
- **Temporary manual test CLI** (Phase 1b) to hand-send a `SENSOR_REQ` from a paired peer
  so this phase is testable before the master loop exists.
- **Test:** provision worker fingerprints; from a second paired device run the manual CLI
  to subscribe → confirm worker streams and readings arrive at the requester as encrypted
  `SENSOR_ENVELOPE` (not plaintext broadcast — verify on the wire); stop sending → confirm
  **auto-stop after `leaseMs` and that the worker can be re-leased without reboot** (H3
  regression check); send from a paired-but-wrong-fingerprint device → confirm ignored;
  send plaintext → confirm dropped at dispatch; leave fingerprints unprovisioned →
  confirm control accepted from **no one** (L4); invoke `espnowsensorstream on` remotely
  via `espnowremote` **with valid credentials** → confirm rejected as local-console-only
  (D1); sniff the air during an active lease → confirm **no** plaintext sensor frame ever
  appears, only AEAD `SENSOR_ENVELOPE` (D2).

### Phase 1b — Temporary manual CLI (test harness, removed later)

Add a throwaway admin command `espnowsensorreq <MAC> <mask> <mode> <intervalMs>
<leaseMs>` in `espNowCommands[]` that builds a `V4PayloadSensorReq` and sends it to `<MAC>`
over the existing encrypt-or-queue path (same send helper the FS/bond paths use). This
lets a human drive one worker before the master tick exists and is deleted (or left
behind as a diagnostic) once Phase 2 lands.

### Phase 2 — Master fetch tick

- Add the four master `gSettings` fields + rows + commands (§8).
- Add `meshActingAsMaster()` predicate; retrofit the worker "don't stream if master" check
  (`System_ESPNow_Sensors.cpp:927`) to use it (§6).
- Add lease bookkeeping fields (`sensorLeaseSentMs`, `lastReadingMs`) to `MeshPeerMeta`
  (`System_ESPNow.h:217-227`).
- Add the 30s `everyMs`-gated fetch tick in `processMeshHeartbeats`, modeled on the BOND
  SYNC TICK (`System_ESPNow.cpp:8287-8340`), iterating `isMeshPeerAlive` peers and sending
  `SENSOR_REQ`.
- **Test:** enable `espnowsensorfetch on` on the master; confirm all alive paired workers
  begin streaming within 30s and keep streaming; power-cycle a worker → confirm it
  re-arms within one cadence; power off a worker → confirm the master stops re-leasing it
  (no dead-MAC blasts) and the worker (if it comes back) is silent until re-leased;
  confirm the master's `lastReadingMs`-based grace re-subscribe fires only when it truly
  received no envelopes.

### Phase 3 — Failover validation

- Provision both fingerprints on all workers; designate backup on primary
  (`espnow meshbackup`/`backupenable`).
- **Test:** kill the primary; confirm backup promotes (`SYSEVT_MESH_PROMOTED`), its fetch
  tick starts, workers accept its `SENSOR_REQ` via the backup fingerprint with zero worker
  reconfig, **and encrypted `SENSOR_ENVELOPE` replies now flow to the backup** (verify the
  backup's `gRemoteSensorCache` populates, not just that control was accepted — this is the
  H2 control-vs-delivery check); restore primary; confirm demote, hand-back, and replies
  return to the primary.

### Phase 4 — History sink

- Add `espnowSensorFetchHistoryEnable` + a master logging tick draining
  `gRemoteSensorCache` into `/system/sys_logs/remote_sensors.log` via `appendLineWithCap`
  (§7).
- **Test:** enable history; confirm readings append off-RX (no RX stall), file rotates at
  cap, and `gRemoteSensorCache`/`/api/sensors/remote` remain live.

### Phase 5 — Cleanup

- Remove or demote the Phase 1b manual CLI; finalize docs; version bump. (No incremental
  commits during the refactor — finish, user HW-tests, then commit, per repo rule.)

---

## 10. Open risks / decisions deferred

1. **Fingerprint vs MAC-label in the setting.** Recommendation is the full 64-hex Ed25519
   pubkey (via `espnowIdentityFormatPubHex`) stored as a `String`, compared by raw
   32-byte `memcmp` in-handler **after** validating both comparands decode to exactly 32
   bytes (L4). Alternative: add a SHA256-based short fingerprint helper
   (`espnowCryptoHmacSha256`, `System_ESPNow_Crypto.h:71`) for a shorter, human-typable
   setting — deferred; full-hex is unambiguous and needs no new crypto helper. Do NOT
   conflate with the mesh CRC16 "fingerprint" (`meshFingerprintForLabel`,
   `System_ESPNow.cpp:1179-1234`) — unrelated 16-bit mesh-scope value.
2. **History rotation size.** 256KB (reusing `LOG_EVENT_STREAM_CAP`) vs a larger dedicated
   cap; and whether to reuse `System_SensorLogging`'s numbered `.N` rotation
   (`System_SensorLogging.cpp:516-558`) for format-configurable output instead of the
   simpler trim-cap. Deferred to Phase 4 based on observed line size.
3. **Thermal / camera / mic / apds fragmentation.** These 4 types produce no mesh data
   (null builders / 200B frame cap, `System_ESPNow_Sensors.cpp:101`). The mask should
   default to excluding them; if their bits are set they're silently ignored by workers —
   acceptable, but the mask options string (§8) should visually flag them as
   non-streaming to avoid operator confusion.
4. **Multi-master ambiguity.** Two nodes both believing they are master (e.g. a partition
   heals and the primary returns before the backup demotes) would both send `SENSOR_REQ`;
   a worker accepts both (both fingerprints trusted) and the last lease wins — and
   `gSensorControllerMac` follows the last accepted request, so a worker's encrypted
   replies briefly target whichever master last renewed. Harmless for streaming but
   wasteful. Mitigation is the existing demote-on-return path (`:2941-2953`); if it proves
   racy, add a master-side "yield if I hear an authenticated heartbeat from a peer whose
   fingerprint == my configured master" check. Deferred until observed.
5. **Wire-delivery ≠ command-success.** The renewal loop treats "alive but no readings
   within grace" as re-subscribe (§5) rather than assuming a delivered `SENSOR_REQ` armed
   the lease. With encrypted-unicast replies (H2), `lastReadingMs` faithfully reflects that
   *this* controller is receiving. Grace window (`2 × intervalMs`) is a tunable to validate
   on HW.
6. **`intervalMs` semantics.** The worker floors the requested interval at
   `spec.minIntervalMs` (`System_ESPNow_Sensors.cpp:619-623`) and also at
   `gSettings.sensorBroadcastIntervalMs` — so a master's `intervalMs` hint can only slow a
   sensor, never speed it past native cadence. Confirm this is the desired contract (it
   matches the existing human-command behavior).
7. **Standing backup-key authority (residual, L5).** Both provisioned fingerprints are
   equal, always-on controllers with no promotion binding available from the wire. This is
   accepted (it is what makes failover seamless), but it means the backup key must be
   guarded exactly as the primary. If a future wire revision carries a verifiable promotion
   assertion, a worker-side "only accept backup after a signed promotion event" binding
   could shrink the standing blast radius — deferred, no wire field exists today.

---

## 11. Security review resolutions

An adversarial pass reviewed the first draft. Verdict: **sound with fixes**. What it found
and how the final design closes each:

- **H1 — Broader-than-intended remote control path.** *Correction to the review's own
  framing:* the pre-existing `espnowsensorstream on` is **not** reachable by "any paired
  peer" — `v4_handle_cmd` (`System_ESPNow.cpp:5199`) requires the frame to be
  session-encrypted (`:5225`) **and** carry a valid bond token (`:5256`) or valid
  `username:password` for an account on the worker (`isValidUser`, `:5311`). The real gap is
  that *any credentialed (even non-admin, `requiresAdmin=false` at `:14844`) account* could
  invoke it remotely — a strictly broader authority than "the two configured fingerprints,"
  and leaseless (never auto-stops). **Resolved beyond the review's suggestion (decision
  D1):** rather than *gate* the remote branch, we *remove* it — `cmd_espnow_sensorstream`
  becomes local-console-only (rejects `ORIGIN_ESPNOW`, `:5376`). `SENSOR_REQ` is the sole
  remote control path, fingerprint-gated to master/backup. Simpler (no caller-MAC threading)
  and strictly tighter. §1/§2/§8 and the attacker table are updated.
- **H2 — Plaintext data path existed at all.** Reusing `SENSOR_BROADCAST = 150` (plaintext +
  `BROADCAST_AUTH` HMAC — authenticity only, `System_ESPNow.cpp:1521/1882-1935`) would put
  readings in cleartext on-air, readable by any passive sniffer, with delivery tied to a
  `SENSOR` subscription bit decoupled from fingerprint authorization. **Resolved beyond the
  review's suggestion (decision D2):** rather than keep the plaintext path for "local/manual"
  streams, we **delete it entirely**. The only over-the-air sensor-data path is
  session-encrypted unicast `SENSOR_ENVELOPE = 154` (`REQ_SESSION_ENC`) to the authorized
  controller recorded at lease time (`gSensorControllerMac`); manual toggle emits encrypted
  to the current controller or nothing. No sensor data ever leaves a worker in plaintext.
- **H3 — Broadcaster task self-delete wedge.** The draft's lease-expiry snippet called
  `stopSensorDataStreaming` from inside `sensorBroadcasterTask`; for the last leased sensor
  that reaches `stopSensorBroadcaster` → `vTaskDelete(gSensorBroadcasterTask)` on the
  *running* task's own handle, which never returns, so `gSensorBroadcasterTask = nullptr`
  (`System_ESPNow_Sensors.cpp:680-686`) never runs and the worker can never stream again
  until reboot. **Resolved:** the in-task expiry path only clears
  `gSensorStreamingEnabled[i]`/`gSensorLeaseExpiresAt[i]`; after the loop, if nothing
  remains enabled, the task **nulls its own handle first, then breaks to `vTaskDelete(NULL)`**
  — never self-deleting via its cached handle. A re-lease-after-expiry check is added to
  Phase 1 testing.
- **L4 — Empty/malformed configured fingerprint could spuriously match.** An unprovisioned
  `""` fingerprint decoded to a zero-length buffer could `memcmp` 0 bytes and "match."
  **Resolved:** `espnowSensorControlAuthorized()` treats any comparand that is not exactly
  64 hex / 32 decoded bytes as "deny — skip"; if neither master nor backup yields a valid
  32-byte comparand, control is denied. Defaults are `""`, so an unprovisioned worker
  accepts control from no one.
- **L5 — Backup key is a standing, always-trusted controller.** The worker has no
  wire-derived binding to promotion state (`hb.role` unused on RX, `gBackupPromoted` is
  backup-side only), so the backup fingerprint has full control authority at all times, not
  only during failover — doubling the always-trusted controller set. **Resolved (doc-level):**
  §2/§6/§10 now state explicitly that both fingerprints are equal standing controllers and
  the backup key must be protected to the same standard as the primary; a future
  signed-promotion binding is noted as the only way to shrink this, and no such wire field
  exists today.