# ESPNOW V4 — Comprehensive Architecture Plan

**Date:** 2026-05-15
**Supersedes:** [docs/ESPNOW_TRANSPORT_PLAN.md](ESPNOW_TRANSPORT_PLAN.md) (kept for design-evolution history)
**Constraint:** All devices will be wiped and reflashed. **No backwards compatibility.** No tier negotiation, no fallback paths, no migration shims. One firmware version, one wire format, one set of behaviors.

---

## Implementation status (2026-05-17 update)

| Phase | Status | Notes |
|---|---|---|
| 0 — handler-table refactor | ✓ shipped | commit `6705ee3` |
| 1 — V4 wire cutover | ✓ shipped | commit `c6157f2` |
| 2 — multi-mesh data model | ✓ shipped | commits `2fdec6b` through `848a7de` |
| **3 — per-peer crypto (this doc's main content)** | **✓ shipped** | see "Phase 3 retrospective" below; what actually landed differs from the plan in interesting ways |
| 4 — file transfer concurrency | — not started | |
| 5 — event subscription registry | — not started | |
| 6 — UX (web + CLI) | partial | Phase 3 produced several UI improvements (CLI flag widening, "Delivered means delivered" web fix); the broader Phase 6 work is unstarted |

The Phase 3 section below describes the original design. The "Phase 3 retrospective" subsection at the end of that section documents what actually shipped and where it diverged.

---

## TL;DR

V4 is a clean break from V3. The goals are:

1. **Multi-mesh support** — a device can hold N mesh memberships, each with its own passphrase, group key, and peer set; default N = 4.
2. **Forward-secret per-peer crypto** — Signed Ephemeral DH (TLS 1.3-style pattern) for all unicast paths. Mesh broadcasts use the per-mesh group key.
3. **Handler-table dispatch** — opcodes are declarative table entries; new opcodes are additions, not core edits.
4. **Identity flows correctly** — `AuthContext.ip = "espnow:<mac>"` set on every ESPNOW-originated command.
5. **No truncated responses** — CMD output streams in full; CMD_RESP carries exit status only.
6. **Concurrent file transfers** — per-peer-pair slot allocation, serialized flash writes via a single writer task, atomic commit via temp+rename, same-path conflict detection.
7. **Per-peer event subscriptions** — SUBSCRIBE / UNSUBSCRIBE / EVENT opcodes replace broadcast-everything-and-hope.
8. **Mode-orthogonal architecture** — Direct (point-to-point unicast), Mesh (broadcast fan-out), and Bond (privileged 1:1 sync) all sit on the same transport substrate; the crypto/dispatch/storage upgrades apply uniformly without entangling mode-specific logic.

Total scope is ~3 weeks of focused engineering, broken into 7 independently-mergeable phases.

---

## Design principles

These rules are load-bearing — every implementation decision should refer back to them:

1. **One firmware version on the wire.** Wipe-and-reflash means we get to design the protocol for the desired end-state. Use this once-only opportunity for everything that's a wire-format change.
2. **Unicast paths get per-peer session keys with forward secrecy. Broadcasts use the mesh group key.** Per-pair keys make sense between two endpoints; group keys make sense for fan-out. Don't conflate.
3. **Mesh broadcasts authenticate (HMAC with group key) but don't get FS.** Acceptable trade — broadcasts are by definition readable by everyone in the mesh.
4. **Per-task TLS auth identity is already correct** — every command that arrives via ESPNOW installs its `ExecIdentityGuard` correctly today ([System_Utils.cpp:2922](../components/hardwareone/System_Utils.cpp#L2922)). Don't break it. Crypto is transport-layer; identity is application-layer; the two stay orthogonal.
5. **Existing patterns get leveraged, not replaced.** Deferred-to-task dispatch, PSRAM buffer convention, fragmentation, dedup ring, retry queue, JSON-doc-in-PSRAM control opcodes — all keep their shape; new code follows the same patterns.
6. **No silent fallbacks.** Frames at the wrong tier, wrong opcode, wrong session, or wrong mesh get rejected loudly with a log line. Silent acceptance is how you ship downgrade attacks.
7. **Honest about complexity.** The crypto layer is real code with real footguns. Phase boundaries are chosen so the high-risk piece (Phase 3) sits on stable foundations and can be developed in isolation.

---

## V4 wire format

### Frame layout (32-byte header + payload up to 218 bytes = 250 byte ESPNOW cap)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | magic | `0x3148` (`'H1'` little-endian) |
| 2 | 1 | version | `4` |
| 3 | 1 | type | opcode (see table below) |
| 4 | 2 | flags | bit field (see flags below) |
| 6 | 1 | headerLen | `32` |
| 7 | 1 | reserved | must be `0` |
| 8 | 4 | msgId | sender-monotonic, for ACK correlation + dedup |
| 12 | 6 | originMac | original sender for mesh-forwarded frames |
| 18 | 1 | ttl | hops remaining; 0 = drop |
| 19 | 1 | fragIndex | 0-based fragment index |
| 20 | 1 | fragCount | total fragments; 1 = single-frame |
| 21 | 1 | meshId | which mesh (0..N_MESHES-1); 0xFF for broadcast/unauth control |
| 22 | 4 | sessionId | per-peer session identifier; 0 for non-session frames (handshakes, broadcasts) |
| 26 | 4 | frameSeq | session-monotonic frame counter (replay protection) |
| 30 | 2 | crc16 | CRC-CCITT over payload |
| 32 | ≤218 | payload | AEAD-encrypted blob for sessioned/group frames; plaintext for handshake messages |

Notes:
- Header grew from 24 to 32 bytes — payload budget shrinks accordingly. Worth it for the session/mesh/replay fields.
- `sessionId == 0` means "no session" — only used for handshake messages and unicast bootstrap frames. Application-layer opcodes (CMD, TEXT, FILE_*, etc.) must always have `sessionId != 0`.
- `meshId == 0xFF` means "control-plane frame, not associated with any mesh" — used for KEY_EX_HELLO across meshes. After pairing, every frame carries the relevant mesh ID.

### Flags

```
0x0001  ACK_REQ        — sender requests ACK
0x0002  BROADCAST_AUTH — frame is a mesh broadcast, HMAC-authenticated with group key
0x0004  SESSION_FRAME  — payload is AEAD-wrapped with session key
0x0008  HANDSHAKE      — payload is a key-exchange or session message (plaintext, signed)
0x0010  STREAM_BEGIN   — first frame of a stream
0x0020  STREAM_END     — last frame of a stream
0x0040  PRIORITY_HIGH  — bump this above the retry queue (e.g., events)
0x0080  reserved
0x0100  reserved
...
```

Exactly one of `BROADCAST_AUTH`, `SESSION_FRAME`, `HANDSHAKE` must be set on every frame. Anything else is malformed.

### Opcode table (V4)

| Range | Category | Opcodes |
|---|---|---|
| 1–9 | Transport | ACK, NACK, FRAG_REQ, FRAG_REPLY |
| 10–19 | Crypto / pairing | KEY_EX_HELLO, KEY_EX_REPLY, KEY_EX_CONFIRM, SESSION_OPEN, SESSION_CONFIRM, SESSION_CLOSE, SESSION_REKEY |
| 20–29 | Discovery / topology | HEARTBEAT, HEARTBEAT_BROADCAST, TOPO_REQ, TOPO_REPLY, TIME_SYNC |
| 30–49 | Application — unicast | CMD, CMD_RESP_STATUS, TEXT, METADATA_REQ, METADATA_RESP, USER_SYNC, SETTINGS_SYNC |
| 50–59 | Streaming | STREAM_FRAME, STREAM_CTRL |
| 60–69 | Files | FILE_START, FILE_CHUNK, FILE_END, FILE_ACK, FILE_PROGRESS, FILE_CANCEL |
| 70–79 | Events | SUBSCRIBE, UNSUBSCRIBE, EVENT, SUB_LIST_REQ, SUB_LIST_REPLY |
| 80–89 | Sensors (broadcast) | SENSOR_BROADCAST, SENSOR_STATUS |
| 90–99 | Bond | BOND_HEARTBEAT, BOND_CAP, BOND_MANIFEST, BOND_SETTINGS |
| 200–255 | reserved / user-defined | |

Opcodes are stable identifiers — once allocated, never renumbered. Gaps in the ranges are intentional reserves.

---

## Identity & key architecture

### Per-device long-term identity

- One Ed25519 keypair generated at first boot.
- Persisted to `/system/espnow/identity.json`, encrypted at rest with an ESP-IDF efuse-derived key.
- Public key shared during pairing; private key never leaves the device.
- Lost identity = effectively a new device. If NVS is wiped, the device must be re-paired by every peer.

### Per-peer long-term record

Stored at `/system/espnow/peers/<mac>/identity.json` (one file per peer, easy to inspect/wipe individually):

```jsonc
{
  "mac": "AA:BB:CC:DD:EE:FF",
  "meshId": 0,
  "name": "kitchen-sensor",
  "longTermPubEd25519": "<base64 32 bytes>",
  "bondedAtSec": 1715789012,
  "lastSeenSec": 1715789320,
  "bondLevel": 1,
  // Optional metadata, mirrors existing peer metadata schema
  "friendlyName": "Kitchen",
  "room": "Kitchen",
  "tags": ["sensor", "kitchen"]
}
```

### Per-session ephemeral state (RAM only — no NVS writes per session)

```cpp
struct SessionState {
  uint8_t   peerMac[6];
  uint8_t   meshId;
  uint32_t  sessionId;
  uint8_t   aeadKeyTx[32];     // outbound AEAD key (HKDF-derived)
  uint8_t   aeadKeyRx[32];     // inbound  AEAD key (HKDF-derived, separate per direction)
  uint32_t  txSeqNext;         // monotonic outbound counter
  uint32_t  rxSeqHighWater;    // for replay window
  uint64_t  rxSeqBitmap;       // 64-frame replay window
  uint32_t  startedAtMs;
  uint32_t  lastUseMs;
};
```

Sessions live only in RAM. After reboot, peers re-handshake. This gives forward secrecy almost for free: the only thing that survives reboot is the long-term Ed25519 pair, and that key never directly encrypts traffic.

### Per-mesh group state

Stored at `/system/espnow/meshes.json`:

```jsonc
{
  "meshes": [
    {
      "id": 0,
      "label": "primary",
      "passphraseHashPbkdf2": "<base64 32 bytes>",  // store the derived value, not the passphrase
      "enabled": true,
      "default": true
    },
    {
      "id": 1,
      "label": "work",
      "passphraseHashPbkdf2": "<base64 32 bytes>",
      "enabled": true,
      "default": false
    }
  ]
}
```

The PBKDF2-stretched value (e.g., 100k iterations of HMAC-SHA256) is what's stored on disk. The original passphrase is never persisted — only ever held in RAM briefly during entry, then derived and forgotten. The stretched value is used for:

- HMAC authentication during KEY_EX_HELLO / KEY_EX_REPLY.
- Derivation of the mesh **group key** via HKDF with context `"espnow-v4-group-mesh-<id>"` (used for broadcast AEAD).
- Derivation of the mesh **bootstrap MAC key** via HKDF with context `"espnow-v4-bootstrap-mesh-<id>"` (used to HMAC-authenticate KEY_EX messages).

### Boot ordering (critical)

Setup sequence in `setup()`:

1. Initialize PSRAM, filesystem.
2. Load (or generate) long-term Ed25519 identity. **Block ESPNOW init until this succeeds.**
3. Load mesh memberships from `/system/espnow/meshes.json`.
4. Load known peers from `/system/espnow/peers/*`. Populate in-memory peer table.
5. Initialize ESPNOW stack, register handler table.
6. Start espnow_task.
7. Start heartbeat emitter.

If step 2 fails (corrupted NVS), refuse to start ESPNOW and surface a recoverable error via OLED/serial. The user can wipe and re-pair from a known-good state.

---

## Dispatch model (handler-table)

The receive path is split into three layers:

### Layer 1 — frame validation (callback context)

`onEspNowRawRecv()` performs ISR-safe validation:

- Magic & version check.
- Header length check.
- CRC verification.
- Dedup lookup (drop replays).
- Fragmentation reassembly (existing ring-buffer pattern; widen to handle concurrent reassemblies per peer if needed).
- Branch by flags:
  - `HANDSHAKE` → enqueue to handshake-handler queue (deferred).
  - `BROADCAST_AUTH` → verify HMAC with group key, then enqueue to broadcast queue.
  - `SESSION_FRAME` → look up session by `sessionId`, decrypt with `aeadKeyRx`, verify `frameSeq` against replay window, enqueue decrypted payload.

### Layer 2 — handler-table dispatch (espnow_task context)

```cpp
struct V4OpcodeEntry {
  uint8_t    opcode;
  const char* name;
  uint8_t    flags;   // REQ_PAIRED | REQ_SESSION | DEFER_TO_CMD_EXEC | ...
  void     (*handler)(const V4Frame& f, const PeerCtx& src);
};

extern const V4OpcodeEntry kHandlers[];
extern const size_t        kHandlerCount;
```

The dispatcher iterates the table once per frame:

```cpp
bool v4_dispatch(const V4Frame& f, const PeerCtx& src) {
  for (size_t i = 0; i < kHandlerCount; i++) {
    const V4OpcodeEntry& e = kHandlers[i];
    if (e.opcode != f.header.type) continue;
    if ((e.flags & REQ_PAIRED)  && !src.paired)  { logDrop(e, "unpaired"); return false; }
    if ((e.flags & REQ_SESSION) && !src.session) { logDrop(e, "no_session"); return false; }
    e.handler(f, src);
    return true;
  }
  logUnknownOpcode(f.header.type, src.mac);
  return false;
}
```

### Layer 3 — per-opcode handler functions

Each in its own static function, ideally in its own translation unit. Examples:

```cpp
// In System_ESPNow_Handlers_Crypto.cpp:
static void handle_key_ex_hello(const V4Frame& f, const PeerCtx& src);
static void handle_key_ex_reply(const V4Frame& f, const PeerCtx& src);
static void handle_session_open(const V4Frame& f, const PeerCtx& src);
// ...

// In System_ESPNow_Handlers_Files.cpp:
static void handle_file_start(const V4Frame& f, const PeerCtx& src);
static void handle_file_chunk(const V4Frame& f, const PeerCtx& src);
// ...

// In System_ESPNow_Handlers_Events.cpp:
static void handle_subscribe(const V4Frame& f, const PeerCtx& src);
// ...
```

The table is the single source of truth for "what opcodes does this firmware understand." Diagnostic CLI command `espnowopcodes` can dump it.

---

## Per-mode behavior

The same dispatch substrate serves all three operating modes. Differences live in **which opcodes a peer uses**, not in how frames are delivered.

### Direct (point-to-point unicast)

- Frames flow between two specific peers, both in the same mesh.
- All application opcodes (CMD, TEXT, FILE_*, USER_SYNC, METADATA_*) use `SESSION_FRAME` flag with active session keys.
- Discovery via passive HEARTBEAT_BROADCAST listening (existing pattern at [System_ESPNow.cpp:5088](../components/hardwareone/System_ESPNow.cpp#L5088)).
- Pairing via `espnowpair <mac>` triggers KEY_EX handshake.

### Mesh (broadcast fan-out)

- Broadcast opcodes (HEARTBEAT_BROADCAST, SENSOR_BROADCAST, TOPO_REQ/REPLY, TIME_SYNC) use `BROADCAST_AUTH` flag with mesh group key.
- HMAC-authenticated by the group key — receivers know "someone with the mesh key sent this" (which is what mesh trust means).
- Mesh forwarding: TTL decrement on rebroadcast, dedup ring prevents loops (existing pattern preserved).
- TOPO_REPLY entries are filtered by `meshId` — peers in mesh A never report peers in mesh B.

### Bond (privileged 1:1 sync)

- Same wire-level path as Direct.
- Adds the existing capability / manifest / settings sync logic on top (BOND_CAP, BOND_MANIFEST, BOND_SETTINGS opcodes).
- Each mesh can hold one bond pair: `gSettings.bondPeerMac[N_MESHES]` (array, not single string).
- BOND_HEARTBEAT is unicast (not broadcast) — it's a presence signal to the specific bonded peer, not a mesh announcement.

---

## Phase 0 — Handler-table refactor (V3 still on the wire)

**Goal:** Move the dispatch model to handler-table while keeping V3 wire format. Zero behavior change observable from any other firmware.

**Why first:** Every later phase benefits from cleaner dispatch. Doing this first means Phase 1's V4 cutover is a delta on a clean base, not a delta on a tangled if-ladder.

**Scope:**
- Extract V3 wire schema (header struct, opcode enum, payload structs) from [System_ESPNow.cpp:85-232](../components/hardwareone/System_ESPNow.cpp#L85) into new [components/hardwareone/System_ESPNow_Wire.h](../components/hardwareone/).
- Define `V3OpcodeEntry` table in [System_ESPNow_Dispatch.cpp](../components/hardwareone/) (new file).
- Migrate opcodes from the if-ladder to handler functions, one opcode per commit. Keep the if-ladder as fallback during migration; when an opcode is migrated, delete its branch from the ladder.
- After all 32 opcodes are migrated, delete the if-ladder body entirely.
- The recv callback's outer skeleton (validation, fragmentation, deferral) stays in `System_ESPNow.cpp`; only per-opcode logic moves.

**Files touched:**
- [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) — extract handlers
- New: `components/hardwareone/System_ESPNow_Wire.h` — wire schema
- New: `components/hardwareone/System_ESPNow_Dispatch.cpp` — handler table + dispatcher
- New: `components/hardwareone/System_ESPNow_Handlers.h` — handler function declarations

**Verification:** Existing test surface continues to work — bond pairing, remote CLI, file transfer, mesh discovery, all unchanged. Optional: add a unit-test harness that calls `v3_dispatch()` with synthesized frames and asserts the right handler runs.

**Risk:** Low. Each migration is mechanical. Bisectable per-opcode commits.

**Estimate:** 2–3 days.

---

## Phase 1 — V4 wire cutover + identity propagation fixes

**Goal:** Bump to V4, fix `ctx.ip`, lift the 2 KB CMD_RESP cap. Still no crypto changes — frames are still encrypted at the radio layer with passphrase-derived LMK, same as today.

**Scope:**

- Bump magic-stays-same but version byte to `4`. Any received frame with version != 4 is rejected with a log line (no V3 fallback).
- Adopt the new 32-byte header layout. Adopt new opcode numbering from the table above. Wire format changes are total — anything sent or received uses V4 layout.
- In `v3_handle_cmd` (now `handle_cmd`): set `cmd.ctx.auth.ip = String("espnow:") + formatMacAddress(srcMac);`
- Drop the 2 KB `CMD_RESP` truncation. The streaming path (STREAM_BEGIN / STREAM_FRAME / STREAM_END flags on STREAM_FRAME opcode) carries the full command output during execution; the final `CMD_RESP_STATUS` opcode (new) carries only the exit status + a short final message. Verify all `broadcastOutput()` and `Serial.println` calls during command execution route through the stream session, not just buffered output.
- Update CLI command surface to use new opcode names where they map cleanly (`bondconnect`, `espnowsend`, etc. are user-facing names, untouched).

**Files touched:**
- [components/hardwareone/System_ESPNow_Wire.h](../components/hardwareone/) — V4 header struct & opcode enum
- [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) — version checks, `handle_cmd` identity fix, response routing
- [components/hardwareone/System_ESPNow_Handlers.h](../components/hardwareone/) — new handler signatures
- [components/hardwareone/System_Utils.cpp](../components/hardwareone/System_Utils.cpp) — verify streaming covers full output

**Verification:**
- Manual: pair two reflashed devices, run `espnowremote <peer> meshstatus`, confirm full output arrives (not truncated).
- Logs show `espnow:AA:BB:CC:DD:EE:FF` as the IP for inbound commands.
- `cmd_espnow_startstream` gate at [:9483](../components/hardwareone/System_ESPNow.cpp#L9483) now correctly accepts ESPNOW-originated invocations and rejects others.

**Risk:** Medium. Wire format change touches every send/recv site. Mitigation: phase 0's handler-table refactor isolates the per-opcode handlers, so each one is updated in isolation.

**Estimate:** 3–4 days.

---

## Phase 2 — Multi-mesh data model

**Goal:** Storage and routing for N mesh memberships per device. No crypto changes; no UX yet (Phase 6 brings UX).

**Scope:**

- Replace `gSettings.espnowPassphrase` (String) with `gSettings.meshes[N_MESHES]` (array of `MeshIdentity` structs).
- `MeshIdentity` fields: `id`, `label`, `passphraseHashPbkdf2`, `enabled`, `isDefault`.
- `EspNowDevice` gains `meshId` field. Existing methods like `addPeer(mac, name)` get a `meshId` parameter; default value (0) for legacy call sites.
- `gSettings.bondPeerMac` (String) becomes `gSettings.bondPeerMac[N_MESHES]` (array). Same for `bondRole`, `bondModeEnabled`.
- Frame's `meshId` field (byte 21 of header) gets populated on send from the peer's record; rejected on receive if it doesn't match a known mesh on this device.
- Topology discovery filtered by mesh: TOPO_REPLY only reports peers in the same mesh as the requester.
- Heartbeat broadcasts go out per-mesh: a device in two meshes emits two heartbeat streams (or one heartbeat with a mesh-bitmap in the payload — the latter is more efficient).

**Defaults & migration (clean wipe):**

- Fresh device starts with no meshes. First boot UX (Phase 6) prompts the user to create or join one.
- Most users have exactly one mesh, configured as `meshes[0] = "primary"`.

**Files touched:**
- [components/hardwareone/System_Settings.h](../components/hardwareone/System_Settings.h) — schema change
- [components/hardwareone/System_ESPNow.h](../components/hardwareone/System_ESPNow.h) — `MeshIdentity` struct, `EspNowDevice` gains `meshId`
- [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) — peer table operations, heartbeat emission, topology
- All `addPeer` / `setPeer` call sites get a `meshId` argument

**Verification:**
- Bring up one device with two meshes configured. Pair peers into different meshes. Confirm:
  - Heartbeats from mesh A peers never show up in mesh B's peer list.
  - Topology discovery within mesh A doesn't reveal mesh B peers.
  - Bond mode can operate in mesh A and mesh B simultaneously (one bond peer per mesh).

**Risk:** Low. Pure data-model change. Settings serialization needs care but the JSON format already handles arrays.

**Estimate:** 2–3 days.

---

## Phase 3 — Per-peer cryptographic identity (Signed Ephemeral DH)

**Goal:** Forward-secret per-pair session keys for unicast. Mesh broadcasts use the per-mesh group key (PBKDF2-derived).

**The protocol in detail:**

### Long-term identity bootstrap (first boot, one-time)

- Generate Ed25519 keypair via `mbedtls_pk_setup` / `mbedtls_pk_genkey`.
- Persist to `/system/espnow/identity.json`, encrypted at rest using ESP-IDF efuse-derived key.

### Pairing (KEY_EX flow)

Triggered by `espnowpair <mac> [<mesh>]`. Both devices must have the same mesh passphrase configured.

```
Device A → Device B:  KEY_EX_HELLO
  payload: {
    meshId,
    A's longTermPubEd25519,
    HMAC over (meshId || A's pub) using PBKDF2(mesh passphrase)
  }

Device B verifies HMAC. If valid:
  Persist A's longTermPub keyed by (meshId, A's MAC) to /system/espnow/peers/<mac>/identity.json

Device B → Device A:  KEY_EX_REPLY
  payload: {
    meshId,
    B's longTermPubEd25519,
    HMAC over (meshId || B's pub) using PBKDF2(mesh passphrase)
  }

Device A verifies HMAC, persists B's longTermPub.

Both devices proceed to SESSION_OPEN (next subsection).
```

### Session establishment (SESSION_OPEN / SESSION_CONFIRM)

Triggered: (a) first use after pairing, (b) reboot, (c) periodic rotation, (d) explicit `espnowrotatekeys`.

```
Device A → Device B:  SESSION_OPEN
  payload: {
    sessionId  (32-bit, generated as random by A),
    ephX25519PubA  (32 bytes, ephemeral),
    nonceA  (16 bytes, random),
    Ed25519_sign(
      sessionId || ephX25519PubA || nonceA || B's MAC,
      A's longTermPrivEd25519
    )
  }

Device B:
  Verifies signature using stored A's longTermPub.
  Generates ephX25519PrivB / ephX25519PubB ephemeral keypair.
  Computes shared = X25519(ephX25519PrivB, ephX25519PubA).
  Derives:
    aeadKeyAtoB = HKDF(shared, "espnow-v4-aead-AtoB", sessionId)
    aeadKeyBtoA = HKDF(shared, "espnow-v4-aead-BtoA", sessionId)
  Stores SessionState in RAM (NOT persisted).
  Discards ephX25519PrivB after key derivation.

Device B → Device A:  SESSION_CONFIRM
  payload: {
    sessionId,
    ephX25519PubB,
    nonceB,
    Ed25519_sign(
      sessionId || ephX25519PubB || nonceA || nonceB || A's MAC,
      B's longTermPrivEd25519
    )
  }

Device A:
  Verifies signature, discards ephX25519PrivA after key derivation.
  Both sides now hold matching aeadKeyAtoB / aeadKeyBtoA, txSeqNext=0.

Subsequent unicast frames: SESSION_FRAME flag, sessionId in header, frameSeq monotonic,
AEAD-wrap payload with appropriate key (per direction).
```

### Mesh broadcast encryption (no FS, but authenticated)

```
On send:
  Look up mesh group key: HKDF(PBKDF2(passphrase), "espnow-v4-group-mesh-<id>")
  Compute AES-GCM with random 96-bit nonce, AAD = (meshId || msgId || originMac).
  Send with BROADCAST_AUTH flag.

On receive:
  Look up mesh by header.meshId.
  Decrypt with group key; verify AEAD tag.
  If valid, dispatch.
```

### Key rotation

- Default: rotate session keys every 24 hours, or every 100k frames sent (whichever first).
- On rotation: trigger fresh SESSION_OPEN / SESSION_CONFIRM, derive new keys, discard old.
- In-flight frames during rotation: receiver tolerates old session for a 5-second grace period after new session established.

### Concurrent handshake tiebreak

If A and B both send SESSION_OPEN simultaneously: tiebreak by MAC — lower MAC's SESSION_OPEN wins, higher MAC abandons its own and responds to the winning one.

### Replay protection per-session

`SessionState.rxSeqBitmap` is a 64-frame sliding window. Any frame with `frameSeq` below the window's low edge is rejected. Frames within the window are accepted only if not already seen. Frames above the window advance it.

**Files touched:**
- New: [components/hardwareone/System_ESPNow_Crypto.cpp](../components/hardwareone/) — Ed25519, X25519, HKDF, AEAD wrappers
- New: [components/hardwareone/System_ESPNow_Identity.cpp](../components/hardwareone/) — long-term identity gen/load/persist
- New: [components/hardwareone/System_ESPNow_Sessions.cpp](../components/hardwareone/) — SessionState management, handshake flow
- [components/hardwareone/System_ESPNow_Dispatch.cpp](../components/hardwareone/) — register crypto opcodes
- [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) — send-path session wrap, recv-path session unwrap

**mbedtls config:** ensure `MBEDTLS_ECDH_C`, `MBEDTLS_ECDSA_C`, `MBEDTLS_HKDF_C`, `MBEDTLS_GCM_C`, `MBEDTLS_ECP_DP_CURVE25519_ENABLED`, `MBEDTLS_PBKDF2_C` are enabled in `sdkconfig`.

**Verification:**
- Two reflashed devices, fresh identities, fresh pairing. Confirm KEY_EX completes; persisted peer records on disk; SESSION_OPEN completes; subsequent CMD frame works.
- Capture a frame on-air with `esp_now_promiscuous_rx_cb` (debug build), confirm payload is AEAD-encrypted (random-looking).
- Replay test: capture a frame, re-inject, confirm rejection.
- Reboot one device, confirm session re-establishes, prior session frames rejected.
- Rotation test: trigger `espnowrotatekeys`, confirm new sessionId, old session rejected.
- Wrong-passphrase test: device with bad passphrase tries KEY_EX_HELLO; receiver rejects HMAC fail.

**Risk:** High. Crypto code is the most failure-prone piece. Mitigations:
- All cryptographic primitives via mbedtls — no homebrew.
- Per-opcode handlers in their own translation unit, unit-testable in isolation.
- Phase 3 sits on stable foundations (handler-table from Phase 0, V4 wire from Phase 1, multi-mesh data from Phase 2) — no concurrent moving pieces.

**Estimate:** 5–7 days.

---

## Phase 3 retrospective (2026-05-17)

Phase 3 shipped across ~25 commits over a long evening of focused work. Field-verified on two ESP32-PICO boards. The major design lines all held; several details changed during implementation. Documented here so the next person reading this codebase understands why what shipped doesn't exactly match what's described above.

### What shipped (sub-phase breakdown)

| Sub-phase | What | Commit (initial) |
|---|---|---|
| 3.0 | Long-term Ed25519 identity, generate/persist, CLI (`espnowidentity` / `espnowregenidentity`) | `f0ac821` |
| 3.1 | PBKDF2-HMAC-SHA256 passphrase stretching, Blake2b KDF for mesh subkeys, in-RAM cache | `f0ac821` |
| 3.2 | Per-peer identity persistence at `/system/espnow/peers/<MAC>/identity.json` | `f0ac821` |
| 3.3 | KEY_EX_HELLO/REPLY/CONFIRM handshake (HMAC-authenticated via mesh bootstrap key) | `f0ac821` |
| 3.4 | SESSION_OPEN/CONFIRM (SIGMA-I pattern: Ed25519-signed ephemeral X25519 DH), `SessionState` table, replay window | `f0ac821` |
| 3.5 step 1+2 | Opt-in `--encrypt` flag on `espnowsend`, passive RX unwrap of `SESSION_FRAME` | `f0ac821` |
| 3.5 step 3 | Default-encrypt for TEXT, auto-kick `SESSION_OPEN` + per-peer pending-frame queue | `ef4e77b` |
| 3.5 step 4 | LMK removal (radio-layer encryption stripped; AEAD is the only confidentiality layer now) | `ef4e77b` |
| 3.5 task #49 | UI "Delivered" semantics: piggyback ACK-driven delivery state on existing `/api/espnow/messages` poll, no SSE | `981c6d1` |
| 3.5 task #50 (#4-now) | `ESPNOW_V4_MAX_PLAINTEXT = 202` constant, shrink encrypt-eligible structs, static_assert | `63e4584` |
| 3.6 | SESSION_REKEY (symmetric two-message exchange, prev-keys 5s retention window, auto-trigger on 10k frames / 1h age) | `8fcb8e4` |
| 3.5 task #32 | BROADCAST_AUTH HMAC on every mesh broadcast (auth-only, plaintext payload) | `61281b3` |
| 3.5 task #6 | Default-encrypt for CMD, CMD_RESP, USER_SYNC, METADATA_* (single-frame opcodes) | `61281b3` |
| 3.5 A1+A2 | Encrypted CMD_RESP at the legacy callsites + per-chunk encrypted STREAM | _this commit_ |

### Design decisions that changed during implementation

**Crypto library: libsodium instead of mbedtls.** The plan said "All cryptographic primitives via mbedtls — no homebrew." We ended up using libsodium for Ed25519 / X25519 / ChaCha20-Poly1305 / Blake2b, and mbedtls only for PBKDF2-HMAC-SHA256 (hardware SHA acceleration). Reason: libsodium has a cleaner one-shot API for the operations we needed; mbedtls's PK setup ceremony was clunky for our use case. We pay a binary-size cost (~80 KB libsodium static) which is fine on 8 MB flash.

**AEAD: ChaCha20-Poly1305 instead of AES-GCM.** Plan said GCM. Switched because libsodium's AES-GCM implementation has a CPU-feature-check pitfall on ESP32 (no AES-NI); ChaCha20-Poly1305 has a simpler portable implementation and runs at full speed without hardware acceleration.

**KDF: Blake2b subkey derivation instead of HKDF.** Plan said HKDF. libsodium provides `crypto_kdf_blake2b_derive_from_key` which is functionally equivalent for our needs (per-direction key derivation from a shared secret) and avoids dragging in the HKDF dependency.

**SessionState is RAM-only, but reboots survive better than expected.** Plan said sessions vanish on reboot. They do — but the long-term identity (3.0) and peer identity files (3.2) survive, so post-reboot session re-establishment is one SESSION_OPEN round-trip away. The user doesn't re-pair.

**Concurrent-handshake tiebreak: SIGMA-I converges, no tiebreak needed.** Plan said "lower MAC wins." Turns out the SIGMA-I exchange converges naturally — if both sides initiate simultaneously, they each treat the peer's REKEY as the responder's reply, derive shared from the same (ephA, ephB) pair, end up with identical keys. No tiebreak logic needed.

**Stack management: defer to cmd_exec_task, not bump espnow_task.** We discovered during 3.4 that Ed25519 sign+verify uses ~3 KB libsodium internal stack each, and SESSION_OPEN runs verify→keygen→ECDH→KDF→sign sequentially. We initially bumped `ESPNOW_HB_STACK_WORDS` 22→30 KB. Then we refactored to PSRAM-copy the payload and `submitDeferredToCmdExec()` — the heavy crypto runs on cmd_exec_task (24 KB, deeper, single-threaded). Reverted the stack bump. Net DRAM cost: −8 KB vs. the initial fix.

**Auto-kick + queue: had to be invented.** Plan implied "session establishment is a separate user step." But forcing the user to manually run `espnowsessionopen` before every encrypted send was unacceptable UX. We added a 4-slot per-peer pending-frame queue. When `v4_send_encrypted_or_queue` sees no active session, it queues the frame and kicks `SESSION_OPEN`. The deferred `runDeferredSessionConfirm` drains the queue on success. 5-second timeout on stuck queued frames. The drain also fires on `sessionApplyRekeyedKeys` (Phase 3.6 follow-up).

**REKEY: symmetric two-message, not three-message confirm.** Plan said "Default: rotate session keys every 24 hours, or every 100k frames sent." We shipped this but with simpler protocol than the SESSION_OPEN/CONFIRM pattern would suggest: a single `SESSION_REKEY` opcode, each side sends one carrying its fresh ephemeral pub, both derive shared. Concurrent rekeys converge via the same shared-ECDH property. Plus a prev-keys retention window so in-flight frames sent under old keys decrypt cleanly.

**LMK removal was a deliberate "no fallback" decision.** Plan implicitly assumed coexistence. We stripped LMK entirely in step 4 — every peer is added with `peerInfo.encrypt = false` regardless of arguments. Confidentiality is now AEAD-only. Discovered a regression where the METADATA_PUSH loop was gated on `peer.encrypt` (always false post-rip), making it dead code. Fixed in the same commit by switching the gate to "has peer identity?"

### Pre-existing bug surfaced & fixed during Phase 3

**`v4_send_frame` flags parameter was `uint8_t` but the flags enum is `uint16_t`.** Every send with a high-byte flag bit (BROADCAST_AUTH=0x0100, SESSION_FRAME=0x0200, HANDSHAKE=0x0400) silently truncated to `0x00`. We confirmed in earlier Phase 3.4 logs that SESSION_OPEN was being sent with `Flags=0x01` instead of `0x0401`. The handshake worked anyway because dispatch is keyed on `type`, not `flags`. Widening to `uint16_t` (one-line change, three declarations) fixed three protocol-correctness issues at once. Caught only because BROADCAST_AUTH would have had the same fate.

### What's deferred (intentionally not in scope)

- **Encrypted fragmentation (#51).** Plaintext payloads > 202 B still go through `v4_send_chunked` plaintext fragmenter. Affects: FILE_DATA, large CMD_RESP output, large METADATA. Plan: per-fragment SESSION_FRAME wrap (Design X), drops plaintext-per-fragment 200→184 bytes.
- **Bond mode token re-derivation.** Bond is the auth/RCE channel between paired devices; it has its own token that should be re-derived off the new Ed25519 identity. Separate workstream — bond's threat model differs from general messaging.
- **Encrypted broadcasts.** Current BROADCAST_AUTH is auth-only (HMAC tag on plaintext payload). Broadcast confidentiality would require group-key AEAD + per-sender replay state on receivers. Locked decision to keep broadcasts plaintext: heartbeat / sensor data are inherently observable from a sniffer regardless of which AP the device is associated with.

### Field-verification summary

Two-device end-to-end tests covered:
- Identity persistence across reflash (Ed25519 keypair survives)
- KEY_EX completes, peer pubkeys cross-persist
- SESSION_OPEN establishes a session in ~250 ms
- Cold-path encrypted send (no session): queue → kick → drain in ~280 ms
- Warm-path encrypted send: immediate SESSION_FRAME with `Flags=0x201, PayloadLen=plaintext+16`
- REKEY: ~12 sequential rekeys in rapid succession without crashes, AEAD failures, or replay rejects
- BROADCAST_AUTH: every heartbeat shows `Flags=0x101`, no HMAC mismatch warnings
- Encrypted CMD: `espnowremote <peer> <user> <pass> <cmd>` arrives as `Type=30 Flags=0x201` — credentials no longer leak

What's NOT exhaustively verified:
- REKEY auto-trigger (thresholds set to 10k frames / 1h; never hit in test)
- Concurrent rekey race (both sides initiate within RTT window) — design-correct, untested
- Prev-keys fallback window — reachable in principle, untested (timing-sensitive)

### Memory budget

Phase 3 RAM cost (approximate):
- `gPeerIdentities[N]`: 16 slots × ~50 B = ~800 B PSRAM
- `gSessions[16]`: 16 × 192 B = 3072 B PSRAM (bumped from 128 B/slot for REKEY prev-keys state)
- `gPending[4]`: 4 × ~220 B = ~880 B PSRAM
- `gSendStatus[16]`: 16 × ~30 B = ~480 B PSRAM
- `gMeshKeys` cache: per-mesh PBKDF2 hash + 2 subkeys + meta = ~150 B per mesh × N meshes
- libsodium static state: ~10 KB DRAM/IRAM

Binary size grew from ~3.9 MB (pre-Phase-3) to ~4.16 MB (post-Phase-3). Mostly libsodium.

---

## Phase 4 — File transfer concurrency

**Goal:** Replace global `gFileTransferLocked` with per-slot allocation. Multiple peers can have transfers in flight simultaneously. Storage I/O remains serialized.

**The safe pattern: parallelize accumulation, serialize flush.**

### Slot structure (PSRAM-allocated)

```cpp
struct FileTransferSlot {
  uint8_t   peerMac[6];
  uint32_t  msgId;
  uint8_t   meshId;
  String    destPath;        // final path
  String    tempPath;        // /tmp/.transfer-<msgId>.part
  uint32_t  totalBytes;
  uint32_t  receivedBytes;
  uint32_t  crc32;           // accumulator
  uint8_t*  bufferPSRAM;     // accumulation buffer (4 KB)
  size_t    bufferUsed;
  uint32_t  lastFrameMs;     // for timeout
  enum { FREE, RECEIVING, WRITING, COMPLETING, FAILED } state;
  SemaphoreHandle_t lock;    // protects slot during cross-task access
};

static FileTransferSlot gFileSlots[N_FILE_SLOTS];  // N_FILE_SLOTS = 4
```

### Allocation

- On FILE_START: walk `gFileSlots[]`, find first FREE slot, claim. If none free, reply `FILE_BUSY`.
- Check for same-path conflict: walk slots, if any has `destPath == requested && state != FREE`, reject with `FILE_PATH_BUSY`.

### Receive path

- FILE_CHUNK arrives: look up slot by `(peerMac, msgId)`. If no match, drop. If found, append to `bufferPSRAM` (under slot lock).
- When buffer hits 4 KB or transfer completes, slot transitions to WRITING.

### Single writer task

- A new `file_writer_task` (FreeRTOS task, stack ~8 KB, priority slightly below espnow_task).
- Round-robin scan of slots: any slot in WRITING state? Drain its PSRAM buffer to the temp file on disk.
- LittleFS file handle opened once per slot when WRITING starts; closed when COMPLETING.
- After write, slot transitions back to RECEIVING (or COMPLETING if `receivedBytes == totalBytes`).

### Completion (FILE_END received)

- Slot transitions to COMPLETING.
- Writer task drains last buffer, fsync, closes handle.
- Atomic rename: `rename(tempPath, destPath)`.
- Slot frees, FILE_PROGRESS final frame sent to peer with success flag.

### Failure modes

- **Timeout** (`millis() - lastFrameMs > 30000`): writer task detects, deletes temp file, frees slot, sends FILE_CANCEL to peer.
- **Peer disconnect**: when a peer is unpaired or session lost, all its active slots get FILE_CANCEL'd.
- **Reboot mid-transfer**: temp files remain in `/tmp/.transfer-*.part`. Boot-time cleanup deletes all such files (the final destPath was never touched, so no corruption).
- **Filesystem full**: writer task gets ENOSPC, slot transitions to FAILED, peer notified, temp file deleted.

### Permissions

- Each slot's writes go through `VFS::openGuarded(tempPath, "w", ctx)` where `ctx` is the auth context of the originating CMD. The existing per-task TLS identity machinery enforces write permission.

### Concurrency guarantees

- Storage I/O is **serial** (single writer task, single LittleFS lock).
- Network reception is **concurrent** (per-slot PSRAM buffers, no shared mutable state in the recv callback path).
- **No race for the destination path**: atomic rename means the final path either exists with complete contents, or doesn't exist at all. Never half-written.

### Memory budget

- 4 slots × 4 KB buffer = 16 KB PSRAM. Each slot's struct is ~256 bytes. Total: 17 KB PSRAM.

**Files touched:**
- New: [components/hardwareone/System_ESPNow_Files.cpp](../components/hardwareone/) — slot management, writer task
- [components/hardwareone/System_ESPNow.h](../components/hardwareone/System_ESPNow.h) — slot struct declaration
- [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) — remove `gFileTransferLocked`
- New opcodes: FILE_PROGRESS, FILE_CANCEL (FILE_ACK already exists)

**Verification:**
- Two peers initiate uploads to different paths simultaneously. Both complete.
- Two peers initiate uploads to the same path. First succeeds, second gets `FILE_PATH_BUSY`.
- One peer initiates 5 transfers simultaneously (more than slot count). 4 proceed, 5th gets `FILE_BUSY`.
- Reboot during a transfer. After reboot, destination path is unchanged; temp file gone.
- Slow peer test: send a transfer with a 60s gap between chunks. Slot times out at 30s; FILE_CANCEL sent.
- Permission test: low-privilege user attempts transfer to a path they can't write. Slot fails open due to `VFS::openGuarded` denying.

**Risk:** Medium. Storage I/O has interesting failure modes. Mitigations: extensive failure-mode testing; explicit handling of every error path before declaring done.

**Estimate:** 3–4 days.

---

## Phase 5 — Event subscription registry

**Goal:** Per-peer subscription to specific event classes. Replace "broadcast everything and hope the right peer cares" with explicit interest.

### Event classes

Build on the existing typed events from `/api/events` SSE stream:

| Class | Bit | Notes |
|---|---|---|
| `EVT_SENSOR_STATUS` | 0x0001 | Sensor connection / calibration change |
| `EVT_THRESHOLD` | 0x0002 | Threshold crossed (per-sensor configurable) |
| `EVT_BUTTON` | 0x0004 | Physical or BLE-remote button press |
| `EVT_AUTOMATION` | 0x0008 | Automation rule fired |
| `EVT_ALARM` | 0x0010 | RTC alarm went off |
| `EVT_WIFI_STATE` | 0x0020 | Wi-Fi connected/disconnected |
| `EVT_BLE_STATE` | 0x0040 | BLE peer connected/disconnected |
| `EVT_LOW_BATTERY` | 0x0080 | Battery below threshold |
| `EVT_SYSTEM_ERROR` | 0x0100 | Critical fault |
| `EVT_BOND_STATE` | 0x0200 | Bond peer online/offline |
| `EVT_USER_AUTH` | 0x0400 | Login/logout |
| `EVT_RESERVED_*` | 0x0800–0x8000 | Reserved for future event classes |

### Subscription state

```cpp
struct EventSubscription {
  uint8_t   peerMac[6];
  uint8_t   meshId;
  uint16_t  classMask;       // OR of EVT_* bits
  uint32_t  installedAtMs;
  uint32_t  lastDeliveredMs;
};

static EventSubscription gSubscriptions[N_SUBSCRIPTIONS];  // N_SUBSCRIPTIONS = 32
```

Persisted at `/system/espnow/subscriptions.json` so subscriptions survive reboots.

### Opcodes

- `SUBSCRIBE` { meshId, classMask } — peer requests to subscribe to event classes.
- `UNSUBSCRIBE` { meshId, classMask } — peer un-subscribes (mask of which classes to drop).
- `EVENT` { classBit, payloadJson } — emitter sends to subscribed peers as unicast.
- `SUB_LIST_REQ` — peer asks what classes it's currently subscribed to (sanity check).
- `SUB_LIST_REPLY` { classMask } — response to LIST_REQ.

### Emit path

When the local system fires an event (e.g., a threshold crossing in a sensor task):

```cpp
void emitEvent(uint16_t classBit, const String& jsonPayload) {
  for (auto& sub : gSubscriptions) {
    if (sub.peerMac == nullptr) continue;
    if (!(sub.classMask & classBit)) continue;
    // Unicast EVENT frame to sub.peerMac
    sendEvent(sub.peerMac, sub.meshId, classBit, jsonPayload);
    sub.lastDeliveredMs = millis();
  }
}
```

### Subscription limits

- Cap N_SUBSCRIPTIONS = 32 (sized to match expected fleet size; tunable).
- Each peer can subscribe to multiple classes (one row per peer; mask carries the classes).
- If subscription table is full and a new SUBSCRIBE arrives, reject with `SUB_FULL`.

### Delivery semantics

- **Fire-and-forget.** Events use ACK_REQ flag at the transport layer (so retry queue gives modest reliability), but the emitter doesn't track per-event delivery beyond that.
- **No persistence of undelivered events.** If a peer is offline, events for it are dropped. (Documented as an intentional trade — durable queues are a different system.)
- **High-priority class** for `EVT_SYSTEM_ERROR`, `EVT_ALARM`: set `PRIORITY_HIGH` flag so retry queue prioritizes them.

### Wiring into existing event emitters

Find every site that does broadcast notification today:

- `broadcastNotice()` and similar in `System_Utils.cpp`
- Sensor threshold-cross logic in [`System_Sensors_*.cpp`](../components/hardwareone/)
- Automation rule firing in `System_Automation.cpp`
- RTC alarm in `System_RTC.cpp`
- Wi-Fi state change handlers
- Battery monitor

Each gets one extra line: `emitEvent(EVT_<CLASS>, jsonPayload);` alongside the existing broadcasts. **Doesn't replace existing local broadcasts** — local OLED, web SSE, and BLE notify continue as today. The ESPNOW emit is additive.

**Files touched:**
- New: [components/hardwareone/System_ESPNow_Events.cpp](../components/hardwareone/) — subscription state, opcodes, emit logic
- All event-emission sites across the codebase — add `emitEvent()` call
- Persisted state file `/system/espnow/subscriptions.json` schema

**Verification:**
- Peer A subscribes to EVT_BUTTON. Press button on Peer B. A receives EVENT frame; confirm payload.
- Peer A subscribes, then UNSUBSCRIBE. Subsequent button presses don't deliver.
- 33 peers attempt to subscribe (table size 32). 33rd gets SUB_FULL.
- Reboot, confirm subscriptions reload from disk.
- Multi-class subscription: subscribe to MASK = BUTTON | THRESHOLD. Both classes deliver; others don't.

**Risk:** Medium. Touches many event-emission sites across the codebase. Each is a small addition but easy to miss one. Mitigation: grep audit for known emission patterns, add an audit pass to verify coverage.

**Estimate:** 3–4 days.

---

## Phase 6 — UX (web + CLI)

**Goal:** Surface the new capabilities. CLI for power users, web for everyone.

### CLI commands

- `espnowmeshes` — list configured meshes. Subcommands: `add <label>`, `remove <id>`, `setpass <id>`, `setdefault <id>`.
- `espnowpair <mac> [<mesh>]` — pair, optionally specifying mesh (default: current default).
- `espnowrotatekeys [<peer>]` — force session key rotation for one peer or all.
- `espnowidentity` — show this device's long-term Ed25519 public key (for OOB verification).
- `espnowsubscribe <peer> <classMask>` — subscribe to a peer's events.
- `espnowunsubscribe <peer> [<classMask>]` — unsubscribe.
- `espnowsessions` — show active sessions (peer, sessionId, key age, frame counts).
- `espnowtransfers` — show active file transfers (slot, peer, path, progress).

### Web UI additions

- `/espnow` page: mesh selector at top (toggle which mesh's peer list is shown).
- `/bond` page: per-mesh bond state.
- New `/espnow/meshes` page: add/remove meshes, set passphrases.
- New `/espnow/subscriptions` page: list peers, subscribe/unsubscribe to event classes per peer.
- New `/espnow/identity` page: display long-term pub for the device, show OOB-confirm fingerprint.

### API additions

- `GET /api/espnow/meshes` — list meshes.
- `POST /api/espnow/meshes` — add a mesh.
- `DELETE /api/espnow/meshes/<id>` — remove.
- `GET /api/espnow/subscriptions` — list subscriptions for this device.
- `POST /api/espnow/subscriptions` — install a subscription on a peer.
- `GET /api/espnow/sessions` — list active sessions.
- `POST /api/espnow/rotate-keys` — trigger rotation.

**Files touched:**
- New: [components/hardwareone/WebPage_ESPNow_Meshes.cpp](../components/hardwareone/)
- New: [components/hardwareone/WebPage_ESPNow_Subscriptions.cpp](../components/hardwareone/)
- [components/hardwareone/WebPage_ESPNow.cpp](../components/hardwareone/WebPage_ESPNow.cpp) — mesh selector
- [components/hardwareone/WebPage_Bond.cpp](../components/hardwareone/WebPage_Bond.cpp) — per-mesh state
- [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) — new CLI command implementations

**Verification:** Manual smoke tests across all new pages and commands. No new automated tests strictly required.

**Risk:** Low. UI work.

**Estimate:** 3–4 days.

---

## Phase ordering & dependencies

```
Phase 0 (handler table)
   ↓
Phase 1 (V4 wire) ─────────┬──────────────┐
   ↓                       ↓              ↓
Phase 2 (multi-mesh)     Phase 4       Phase 5
   ↓                     (files)       (events)
Phase 3 (crypto)
   ↓
Phase 6 (UX)
```

- Phase 0 unblocks everything else by isolating per-opcode logic.
- Phase 1 establishes V4 wire format. Phases 2, 4, 5 can proceed in parallel after Phase 1.
- Phase 3 depends on Phase 2 (mesh-scoped keys) and Phase 1 (V4 wire).
- Phase 6 depends on everything below it for surface to expose.

If a single contributor, work sequentially: 0 → 1 → 2 → 3 → 4 → 5 → 6.
If two contributors, after Phase 1: one takes 3, the other takes 4 and 5 in series, both converge at Phase 6.

---

## Total estimate

| Phase | Days | Risk |
|---|---|---|
| 0 — handler table | 2–3 | Low |
| 1 — V4 cutover | 3–4 | Medium |
| 2 — multi-mesh data | 2–3 | Low |
| 3 — crypto | 5–7 | High |
| 4 — file slots | 3–4 | Medium |
| 5 — event subs | 3–4 | Medium |
| 6 — UX | 3–4 | Low |
| **Total** | **21–29 days** | |

Approximately 3–4 weeks of focused single-contributor engineering.

---

## Footguns and decisions to lock in early

### Concurrent handshakes

If A and B initiate SESSION_OPEN to each other simultaneously, both sides see an incoming SESSION_OPEN while their own is in flight. Without a tiebreak, both could establish two different sessions. **Decision:** lower MAC wins. The higher-MAC device abandons its own SESSION_OPEN and responds to the lower-MAC peer's incoming one. Logged at debug level for diagnostic visibility.

### Boot-time identity load failure

If `/system/espnow/identity.json` is corrupt or unreadable at boot, the device cannot pair or session. **Decision:** refuse to start ESPNOW. Surface error via OLED. CLI command `espnowregenidentity` regenerates (with warning that all existing bonds are invalidated).

### NVS wear from key rotation

Sessions live in RAM, so rotation doesn't touch NVS. **No wear concern for sessions.** Long-term identity is written once. Per-peer records are written once per pairing and on metadata updates. Subscription state is the only thing that might churn — cap writes to once-per-N-seconds via a debounce.

### Clock skew across mesh

Devices don't have synced wall clocks unless TIME_SYNC has propagated. **Decision:** all freshness checks in handshakes use random nonces, not timestamps. The replay window in SessionState is monotonic frameSeq, not wall-clock based.

### Mesh ID conflicts on join

A device joining mesh A doesn't know what mesh ID local peers are using internally — mesh IDs are local indices. **Decision:** label is what matters globally (e.g., "primary", "work"); ID is just a local array index. Two devices in the same mesh might have different IDs for it locally — that's fine because frames carry the meshId from the sender's local table, which the receiver maps to its local label.

Wait — that's a problem. Let me think again.

Actually: meshId in the frame header refers to the **sender's local index**. The receiver doesn't know what the sender means by `meshId=2`. Two options:

1. **Use mesh fingerprint instead of index.** Hash the mesh label into a 32-bit ID; both devices compute the same fingerprint from the same label. Frame carries the fingerprint, receiver looks up by fingerprint. **This is the right answer.** Update the wire format: replace `meshId` (1 byte) with `meshFingerprint` (4 bytes, low byte of SHA256(label)). Receiver maintains a fingerprint→local-index map.

2. **Negotiate IDs at pairing.** More complex, more state. Don't bother.

**Decision:** use fingerprint, not local index, in the frame header. Adjust header layout: byte 21 becomes 3 bytes of `meshFingerprint`, and one of the reserved bytes makes up the difference. Header stays at 32 bytes.

### Subscription explosion

A peer subscribed to `EVT_THRESHOLD | EVT_BUTTON` on a noisy sensor device could receive thousands of events per minute. **Decision:** rate-limit per (peer, class) at the emit side. Default: max 10 events per class per peer per second; configurable.

### Forward compatibility with future opcodes

Even though we're rejecting backwards compat, future versions of V4 may want to add opcodes. **Decision:** unknown opcodes are logged and dropped (never crash). Reserved opcode ranges in the table give future versions room.

### What about `bondPeerMac` for cross-mesh bonds?

A device could in principle bond with a peer in mesh A and another peer in mesh B. **Decision:** allow it. `gSettings.bondPeerMac[N_MESHES]` is an array. Bond mode state machine runs per-mesh.

### Cross-mesh bridging

If a device is in two meshes, can it relay frames between them? **Decision: NO, not in V4.** Bridging is a security-sensitive feature that deserves its own design doc. Mesh isolation is enforced: a frame received on mesh A is never re-emitted on mesh B regardless of opcode.

### USER_SYNC and mesh boundaries

USER_SYNC propagates user credentials between bonded peers. If a device is in two meshes, does USER_SYNC cross mesh boundaries? **Decision: NO.** USER_SYNC respects mesh scope. Users in mesh A's bond pair are synced; users in mesh B's bond pair are synced; never cross.

---

## Verification approach

Per-phase smoke tests are listed in each phase section. Cross-phase integration tests:

1. **Two-device pair, full handshake** — wipe both, boot, configure same mesh passphrase, pair, KEY_EX completes, SESSION_OPEN completes, exchange a CMD, verify response (covers Phases 0–3).
2. **Multi-mesh isolation** — device with two meshes, peers in each, confirm no cross-leakage (Phase 2 + 3).
3. **Concurrent file transfers** — two peers upload simultaneously to different paths, both complete (Phase 4).
4. **Event subscription end-to-end** — subscribe, trigger event, receive, unsubscribe, no more deliveries (Phase 5).
5. **Mesh + crypto + files + events together** — a realistic operational scenario: 4 devices in one mesh, each bonded as a pair, each subscribed to a different event class, simultaneous file transfers, periodic key rotations. Run for 24 hours, watch for memory leaks, watch for crashes, audit logs for unexpected drops.

---

## Open questions / decisions needed before coding

These need answers before Phase 1 starts:

1. **N_MESHES default value.** ~~4 covers most cases. 8 is more flexible but costs more PSRAM. Vote: 4 unless there's a strong product reason for more.~~ **DECIDED: `N_MESHES = 4`.**
2. **Session lifetime default.** ~~24h or 100k frames is the proposed default. Should this be user-configurable per-mesh, or global? Vote: global with override per-bond if requested later.~~ **DECIDED: 24h or 100k frames (whichever first), global setting, per-bond override deferred until requested.**
3. **meshFingerprint hash function** (Phase 2). **DECIDED: CRC16-CCITT** — already in the codebase for payload CRC; non-cryptographic (auth comes via group key in Phase 3); collision rate at N=4 meshes is negligible.
4. **Heartbeat emission with multiple meshes** (Phase 2). **DECIDED: separate frame per mesh**, each stamped with its mesh fingerprint. Cleaner than a bitmap-in-one-frame approach: no information leakage about other mesh memberships, independent retry/dedup state per mesh.
5. **Out-of-mesh frame handling** (Phase 2). **DECIDED: silently drop.** If a frame's `meshFingerprint` doesn't match any of our local meshes, drop before any handler sees it. No log spam in environments with many neighbor devices.
6. **PBKDF2 iterations** (Phase 2). **DECIDED: 100k** SHA-256 iterations to stretch the user passphrase. ~1-2 second cost at boot per mesh (only when adding/changing a mesh; cached in NVS otherwise). Worth it for offline-brute-force resistance.
3. **Bonding-window button UX.** Phase 6 mentions web-initiated pairing. Does the user want a physical-button initiation path (long-press a button on each device within 30s)? If yes, that's an additional sub-phase in Phase 6 with maybe +2 days; if no, web/CLI initiation only.
4. **OOB confirmation step.** Display the last 4 hex of the long-term-pub fingerprint on both OLEDs during pairing, require explicit confirm? Recommended for defense against attacker-with-passphrase MITM; adds UX friction. Vote: implement but make opt-in per pairing.
5. **PAKE upgrade path.** The wire format leaves room to swap HMAC-authenticated KEY_EX for J-PAKE later. Worth implementing now (add ~3 days to Phase 3) or defer? Vote: defer. Add only if/when a threat model emerges that justifies it.
6. **TLS-at-rest encryption for `/system/espnow/peers/<mac>/identity.json`.** ESP-IDF efuse-derived key wraps it. Worth doing now, or accept that physical access to NVS means full compromise? Vote: do it now — cost is one mbedtls call per read/write, negligible.
7. **Diagnostic CLI: `espnowdump`.** A privileged command that dumps current session state, peer table, mesh memberships, subscriptions for support purposes. Vote: yes, admin-only, redact key material.

---

## Files touched (master list)

### New files

- `components/hardwareone/System_ESPNow_Wire.h` — V4 header + opcode definitions
- `components/hardwareone/System_ESPNow_Dispatch.cpp` — handler table + dispatcher
- `components/hardwareone/System_ESPNow_Handlers.h` — handler function declarations
- `components/hardwareone/System_ESPNow_Crypto.cpp` — Ed25519, X25519, HKDF, AEAD wrappers
- `components/hardwareone/System_ESPNow_Identity.cpp` — long-term identity management
- `components/hardwareone/System_ESPNow_Sessions.cpp` — SessionState + handshake flow
- `components/hardwareone/System_ESPNow_Files.cpp` — slot management + writer task
- `components/hardwareone/System_ESPNow_Events.cpp` — subscription registry + emit
- `components/hardwareone/System_ESPNow_Handlers_Crypto.cpp` — KEY_EX, SESSION_*, REKEY handlers
- `components/hardwareone/System_ESPNow_Handlers_Files.cpp` — FILE_* handlers
- `components/hardwareone/System_ESPNow_Handlers_Events.cpp` — SUBSCRIBE, EVENT handlers
- `components/hardwareone/WebPage_ESPNow_Meshes.cpp` — meshes admin UI
- `components/hardwareone/WebPage_ESPNow_Subscriptions.cpp` — subscriptions admin UI

### Modified files

- `components/hardwareone/System_ESPNow.cpp` — substantial; old monolithic handler logic moves out
- `components/hardwareone/System_ESPNow.h` — schema additions (`MeshIdentity`, `SessionState`, `FileTransferSlot`, `EventSubscription`)
- `components/hardwareone/System_Settings.h` — `meshes[N_MESHES]`, `bondPeerMac[N_MESHES]`
- `components/hardwareone/System_Utils.cpp` — verify streaming path covers full CMD output
- `components/hardwareone/WebPage_ESPNow.cpp` — mesh selector
- `components/hardwareone/WebPage_Bond.cpp` — per-mesh bond state
- All sensor/event emission sites — add `emitEvent()` calls
- `components/hardwareone/CMakeLists.txt` — register new files
- `sdkconfig.defaults.*` — enable mbedtls primitives

### Deleted

- The if-ladder body in `v3_try_handle_incoming()` (after Phase 0 completes migration).
- `gFileTransferLocked` and related single-active-transfer logic (Phase 4).
- Any legacy passphrase-LMK fallback code (Phase 1).

---

## Rollback story

Per phase:

- **Phase 0**: pure refactor; revert is `git revert`.
- **Phase 1**: wire format break; once any device flashes V4, V3 devices can't talk to it. Decision: flash whole fleet at once.
- **Phase 2**: schema change; revert needs migration logic to collapse the array back to a single passphrase. Practically: don't roll back; fix forward.
- **Phase 3**: crypto change; revert leaves peers paired-without-keys, all unicast fails. Practically: must roll forward.
- **Phase 4–6**: additive; revert is git revert with no data-loss concern.

The wire format and crypto layers are not reversible after deployment. Treat Phases 1 and 3 as one-way doors and test thoroughly before merging.

---

## Summary

V4 is a substantial but tractable rewrite. It leaves the existing architecture's good ideas intact (deferred-to-task dispatch, PSRAM allocation, per-task TLS auth, mesh dedup/retry/TTL) and replaces only what needs replacing (monolithic if-ladder, missing identity propagation, truncated responses, global file lock, broadcast-only events, no per-pair forward secrecy, no multi-mesh).

The architectural changes compose cleanly:

- Handler table is orthogonal to wire format.
- Wire format is orthogonal to crypto.
- Crypto is orthogonal to file slots and event subscriptions.
- Multi-mesh threads through all of them but is structurally simple (one extra field on the peer record, one extra dimension on settings arrays).

After V4, the architecture is well-positioned for future extensions (PAKE upgrade, group key rotation, cross-mesh bridging) without further protocol breaks.
