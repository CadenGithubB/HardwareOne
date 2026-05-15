# ESPNOW Transport — State of the Subsystem and Prep Plan for Bond / Mesh / Direct

**Date:** 2026-05-15
**Scope:** Audit the existing ESPNOW transport, identify what currently works, what's missing, and lay out a phased plan to make it a first-class transport that exposes the device's full capability surface (CLI, sensors, files, events) over three operating modes: **Direct** (paired peer unicast), **Mesh** (multi-hop fan-out), and **Bond** (privileged 1:1 pairing).

---

## Bottom line up front

The ESPNOW subsystem is **already mature** — `System_ESPNow.cpp` is ~8,000 lines and implements a versioned binary protocol (V3) with 32 opcodes, CRC16, 16-fragment reassembly, ACK/retry, dedup, mesh TTL, topology discovery, time sync, user sync, settings sync, and a working Bond mode for HardwareOne-to-HardwareOne pairing.

The work to enable "transport everything" is therefore not green-field design — it's **surgical refactoring around five clearly identifiable seams**:

1. **Credential-in-payload auth** → move to per-bond asymmetric identities so plaintext passwords stop crossing the air.
2. **Text-only RPC payload** → add a structured binary RPC opcode with chunked response streaming (current 2 KB result cap is a real limit).
3. **Single-active-transfer global lock** → per-peer-pair concurrency for files.
4. **Broadcast-only events** → a subscription registry so a peer can say "stream me IMU + button events" without flooding the mesh.
5. **Monolithic if-ladder dispatch in `v3_try_handle_incoming()`** → opcode handler table so new message types are declarations, not core edits.

Three of the user's three asks already have plumbing today:
- **Direct transport:** works end-to-end (pair via MAC, unicast, ACK, fragmentation).
- **Mesh:** TTL + dedup + retry queue + topology discovery all exist; multi-hop forwarding is partially wired and needs an explicit decrement-and-forward path plus a routing-table abstraction.
- **Bond mode pairing:** **already exists** and is HardwareOne ↔ HardwareOne ESPNOW pairing — `bondconnect <mac>` enables it, capability/manifest/settings sync runs idempotently, web UI at `/bond`. The "prep" needed is mostly UX (button-initiated bonding, LED indicator, discovery wizard) and cryptography (per-peer key instead of shared passphrase).

---

## Current state at a glance

### File layout
| File | Purpose |
|---|---|
| [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) (~8000 lines) | Core transport, V3 protocol, state machine, bond mode, mesh, CLI command implementations |
| [components/hardwareone/System_ESPNow.h](../components/hardwareone/System_ESPNow.h) (~1200 lines) | `EspNowState` global, opcode enums, payload structs, public API |
| [components/hardwareone/System_ESPNow_Sensors.cpp/.h](../components/hardwareone/System_ESPNow_Sensors.cpp) | Sensor broadcast + remote sensor cache |
| [components/hardwareone/WebPage_ESPNow.{h,cpp}](../components/hardwareone/WebPage_ESPNow.h) | Web UI for peer list, pairing wizard, bond status |
| [components/hardwareone/WebPage_Bond.cpp](../components/hardwareone/WebPage_Bond.cpp) | `/bond` page, `/api/bond/{status,stream,exec,role,paired-devices}` |
| [components/hardwareone/OLED_ESPNow.cpp](../components/hardwareone/OLED_ESPNow.cpp), `OLED_RemoteSettings.cpp`, `G2_Page_ESPNow.cpp` | Local-display and glasses-page surface |

### V3 packet schema
24-byte header + 0–226 byte payload (= 250 byte ESPNOW hardware cap). Header fields: magic (0x3148), version, type, flags, headerLen, payloadLen, msgId, origin MAC (mesh provenance), TTL, fragIndex, fragCount, CRC16, reserved. See [`System_ESPNow.cpp:1169`](../components/hardwareone/System_ESPNow.cpp#L1169).

Fragmentation: up to 16 fragments × 200 bytes = **3200-byte max logical message**. Reassembly ring holds 2 slots, 5-second timeout. Dedup ring 64 entries keyed by (origin MAC, msgId).

### Opcodes already implemented
ACK, TEXT, CMD, CMD_RESP, HEARTBEAT, FILE_START/DATA/END, STREAM, STREAM_CTRL, SENSOR_DATA, SENSOR_STATUS, SENSOR_BROADCAST, BOND_HEARTBEAT, METADATA_REQ/RESP/PUSH, TIME_SYNC, TOPO_REQ/START/PEER, USER_SYNC, plus bond-specific capability and settings sync types. **32 in total** as of audit. See enum at [`System_ESPNow.cpp:88`](../components/hardwareone/System_ESPNow.cpp#L88).

### What works end-to-end today
1. **Remote CLI execution.** `espnowremote <peer> <user> <pass> <cmd>` runs an arbitrary CLI command on the peer. The remote-side path runs through the unified `cmd_exec` task and **does install `ExecIdentityGuard` from the inbound auth** ([`System_Utils.cpp:2922`](../components/hardwareone/System_Utils.cpp#L2922)) — so the per-task TLS identity correctly reflects the remote caller during command execution. (This was easy to mistake for missing; it's actually wired.)
2. **Bond mode** between two devices: capability exchange, manifest sync, settings sync, sensor stream toggles, `bondexec` remote commands. Idempotent pull-based design ([`System_ESPNow.cpp:9668` `resetBondSync()`](../components/hardwareone/System_ESPNow.cpp#L9668)).
3. **File transfer** between paired devices via FILE_START/DATA/END with CRC32 ([`System_ESPNow.cpp:1193`](../components/hardwareone/System_ESPNow.cpp#L1193)). Single active transfer at a time (global `gFileTransferLocked`).
4. **Sensor telemetry broadcast** — IMU, GPS, thermal, ToF data shipped as SENSOR_BROADCAST with rate limit `STREAM_MIN_INTERVAL_MS = 100ms`.
5. **Topology discovery** — master broadcasts TOPO_REQ, peers respond with TOPO_PEER frames listing their adjacency.
6. **Time sync** — master broadcasts TIME_SYNC; workers adjust epoch offset.
7. **User sync** — propagate user credentials across bonded peers via encrypted USER_SYNC (`gSettings.espnowPassphrase`-derived key).
8. **Passive peer discovery** — incoming heartbeats from unpaired devices populate `gEspNow->unpairedDevices[]` so the UI can offer a "saw this device — pair?" list ([`System_ESPNow.cpp:5088`](../components/hardwareone/System_ESPNow.cpp#L5088)).
9. **Web + OLED + G2 glasses surface** for peer list, bond status, remote command, settings.

### Auth & crypto model today
- **Frame-level encryption:** optional, opt-in via per-peer flag + per-peer 16-byte key. Key is **derived from a globally-shared passphrase** (`gSettings.espnowPassphrase`) via SHA256-HMAC with salt `":ESP-NOW-SHARED-KEY"`. ESP-IDF's `lmk` slot in `esp_now_peer_info_t` is what carries it.
- **Command auth, payload-level:** either `user:password:command` (plaintext, relying on frame encryption to keep it private) or `@BOND:<token>:command` where token = HMAC-SHA256(passphrase, peerMac || ourMac), 16 bytes hex-encoded. See [`v3_handle_cmd` at System_ESPNow.cpp:3683](../components/hardwareone/System_ESPNow.cpp#L3683).
- **Identity propagation:** validated username flows through `cmd.ctx.auth.user` → `ExecIdentityGuard` → per-task TLS. Bond-token commands run as user `"espnow"` (a synthetic role), not as the originating user.
- **Sender MAC threading:** stashed in `cmd.ctx.auth.opaque` so commands like `cmd_espnow_startstream` can identify their caller ([`System_ESPNow.cpp:9488`](../components/hardwareone/System_ESPNow.cpp#L9488)).
- **Gate at `ip.startsWith("espnow:")`:** [`System_ESPNow.cpp:9483`](../components/hardwareone/System_ESPNow.cpp#L9483) appears to be **dead code** — nothing sets `ctx.ip = "espnow:..."` today. Either remove the gate or wire it.

### Dispatch model
Single RX callback `onEspNowRawRecv()` → `v3_try_handle_incoming()` → a long if-ladder over `h->type`. Most handlers defer to the espnow task via per-opcode buffers in `EspNowState` (deferredCmdPending, deferredCmdRespPending, file-transfer state, stream ring, etc.). Two handlers (ACK, TIME_SYNC) run synchronously in the callback. **New message types currently require editing `v3_try_handle_incoming()` and adding deferred-state fields to `EspNowState`.**

---

## Three transport modes — definitions and current readiness

### Direct (point-to-point unicast)
**State:** Production-ready.
**How it works today:** Add a peer via `espnowpair <mac> <name>`, then unicast frames are routed via `esp_now_send(mac, ...)`. ACK + retry via `ESPNOW_V3_FLAG_ACK_REQ`. Fragmentation transparent up to 3200 bytes.
**What's missing:** A clean C++ API for callers. Today, sending a "structured RPC" means hand-rolling text into the CMD payload. The proposed RPC opcode (below) closes this.

### Mesh (multi-hop fan-out)
**State:** ~70% there.
**What exists:** `ESPNOW_MODE_MESH` flag; per-peer health tracking (`MeshPeerHealth gMeshPeers`); origin-MAC + TTL field; 8-slot retry queue with max 2 retries; 64-entry dedup ring; topology discovery via TOPO_REQ/START/PEER; broadcast tracker for ACK collection across N peers; periodic mesh HEARTBEAT + STATUS frames.
**What's incomplete / unclear:**
- Multi-hop forwarding: TTL is checked (`ttl == 0 → drop`) but I could not find a clean **decrement-and-rebroadcast** path for non-terminal frames. If A→B→C is needed and A can only see B, does B re-broadcast on A's behalf? Worth verifying or implementing.
- No routing table — purely fan-out broadcast with dedup. Fine for ≤10 peers, painful at scale.
- No congestion / backpressure when mesh size grows; STREAM_MIN_INTERVAL_MS is a per-sender rate limit, not a mesh-wide budget.
- Mesh-wide pub/sub doesn't exist — every "broadcast event" goes to every peer; no filtering by interest.

### Bond (privileged 1:1)
**State:** Working but UX-thin.
**What exists:** `bondconnect <mac>` enables bond mode; deterministic role (higher MAC = master); pull-based idempotent sync (master pulls Capability + Manifest + Settings from worker); BOND_HEARTBEAT every N ms; CLI `bondrole`/`bonddisconnect`/`bondstream`/`bondexec`; web UI `/bond` + APIs; bond status surfaces on OLED. Bond peer persists in `gSettings.bondPeerMac` + `gSettings.bondRole` + `gSettings.bondModeEnabled`.
**What's missing for true "Bond mode pairing" as a feature parity with BLE bonding:**
- **No discovery wizard.** User must already know the peer's MAC. The unpaired-device list ([`System_ESPNow.cpp:5088`](../components/hardwareone/System_ESPNow.cpp#L5088)) collects heartbeats from peers in range but there's no "press a button on each device within 30 s to bond" UX.
- **No hardware-button initiation.** Memory note about per-action tasks applies — this should be a state-machine flag toggled by an existing button handler, not a spawned task per press.
- **No LED bond-indicator.** A blink pattern during the pairing window would match user expectations from BLE-style bonding.
- **No per-peer key.** All ESPNOW peers share one passphrase-derived key. A leaked passphrase compromises every bond. Per-peer ephemeral keys negotiated during the pairing window would dramatically improve the security story.
- **Single bond pair only.** The bond-mode state machine assumes one bonded peer at a time. Multi-bond (one device bonded to several peers simultaneously, distinct from generic pairing) would need a list, not a single `bondPeerMac` field.

---

## Capability coverage matrix

What the device can do, by surface, and whether ESPNOW can carry it today.

| Capability | CLI | Web | BLE | ESPNOW today | Gap |
|---|---|---|---|---|---|
| Run arbitrary CLI command | ✓ | ✓ (`POST /api/cli`) | ✓ (CmdRequest chunked) | ✓ (`CMD`/`CMD_RESP` with 2 KB result cap) | Large output truncated; need response streaming |
| Subscribe to event stream | n/a | ✓ (SSE `/api/events`) | ✓ (NOTIFY) | ✗ (broadcast only, no per-peer subscription) | Add SUBSCRIBE/UNSUBSCRIBE opcodes + subscription registry |
| Read sensor snapshot | ✓ | ✓ | ✓ | ✓ (SENSOR_DATA on demand) | OK |
| Stream sensor at rate | n/a | ✓ (SSE) | ✓ | partial (STREAM with 100ms floor, no backpressure) | Add per-peer rate + drop policy |
| Bulk file download | n/a | ✓ | ✗ | ✓ (FILE_START/DATA/END) | Global lock → one at a time |
| Bulk file upload | n/a | ✓ | ✗ | ✓ | Same global lock |
| OTA firmware push | ✗ | ✗ (serial only) | ✗ | ✗ | Out of scope; if added, must use bond-only auth |
| Settings read/write | ✓ | ✓ | ✓ | ✓ (USER_SYNC + bond settings sync) | Generic settings RPC not exposed; bond-specific only |
| Files list / mkdir / rm | ✓ | ✓ | ✗ | partial (via remote CLI) | OK as RPC if RPC layer lands |
| Per-user auth | ✓ | ✓ (sessions) | ✓ (per-conn) | partial (password-in-payload or bond token) | Per-peer cryptographic identity |
| Long-running op progress | n/a | ✓ (SSE) | ✓ | partial (STREAM during CMD) | Verify on chunked CLI output path |
| Discover peers in range | n/a | ✓ (BLE scan UI) | ✓ | partial (passive only via heartbeats) | Active scan + button-initiated pairing window |

The takeaway: **ESPNOW already carries the bulk of the device's capability surface**, with three obvious holes (per-peer subscription, structured RPC with chunked responses, concurrent file transfer) and one architectural concern (the credential model).

---

## Ranked gap list

In rough order of "blocks the most use cases" / "biggest security improvement":

1. **Credential-in-payload auth (security).** Username + password ride the wire. They're encrypted *if* the peer's flag is set and the global passphrase is configured, but the threat model is essentially "anyone with the passphrase can be anyone". Move to per-peer asymmetric identities (ed25519, generated at first bond, persisted in NVS).
2. **2 KB CMD_RESP truncation.** `meshstatus`, `imucalibstatus`, large log dumps, settings exports all exceed this. STREAM frames exist for live output during execution; need to verify they cover the full response, or add a chunked CMD_RESP variant.
3. **No event subscription.** Today, to "subscribe to button presses on peer X" you'd write a remote CLI command that loops, or poll. Need a SUBSCRIBE opcode that maps a peer MAC + event class to a delivery channel.
4. **Global file-transfer lock.** Pipelining and per-pair concurrency are blocked by `gFileTransferLocked` ([`System_ESPNow.cpp:496`](../components/hardwareone/System_ESPNow.cpp#L496)). Convert to a small slab of file-transfer contexts keyed by (peer, msgId).
5. **Text-only RPC payload.** All structured calls go through "remote CLI" — fine for ad-hoc human use, lossy for automation. A binary RPC opcode (msgpack/CBOR or a simple TLV) would let callers ship typed args + receive typed results.
6. **Monolithic dispatch.** Every new opcode = edit `v3_try_handle_incoming()` + add field to `EspNowState`. Move to a handler table indexed by opcode, with per-opcode `(immediate? deferred?)` policy.
7. **Mesh forwarding gaps.** Verify multi-hop A→B→C works in practice; if not, add explicit forward path with TTL decrement.
8. **No multi-bond.** `bondPeerMac` is a single string. Replace with `bondedPeers[]` array if simultaneous bonds matter.
9. **Bonding UX.** Button-initiated pairing window, LED indicator, web wizard improvements.
10. **Dead `ip.startsWith("espnow:")` gate.** Either set the ip prefix in `v3_handle_cmd` or drop the check.

---

## Proposed refactor (phased)

Each phase is **independently shippable** and leaves the system better than it found it. No phase requires a flag-day rewrite.

### Phase 0 — Foundation (1–2 days)
**Goal:** Make the existing code easier to extend without changing behavior.

- Extract the V3 protocol's struct definitions and opcode enum out of `System_ESPNow.cpp` into a header sibling (`System_ESPNow_Wire.h`). Currently they're at lines 85–232 inside the .cpp.
- Introduce a handler-table dispatch shim alongside the existing if-ladder:
  ```cpp
  struct V3HandlerEntry {
    uint8_t type;
    void (*immediate)(...);  // ISR/callback safe, or nullptr
    void (*deferred)(...);   // task context, or nullptr
  };
  ```
  Migrate one opcode (TEXT is the simplest) as a proof. Keep the if-ladder around as fallback during migration.
- Fix the dead `ip` gate: either drop the `startstream` check or set `cmd.ctx.auth.ip = "espnow:" + macStr` in `v3_handle_cmd` (it should be the latter — `opaque` is non-portable raw bytes; `ip` is string-y and shows up in logs).

### Phase 1 — Per-peer cryptographic identity (3–5 days)
**Goal:** Bond mode pairing produces a per-peer key, not a derivative of the global passphrase.

- Add `EspNowPeerKey { uint8_t peerPub[32]; uint8_t selfPriv[32]; uint8_t selfPub[32]; uint8_t sharedSecret[32]; uint32_t bondedAtSec; }` to the peer record, persisted in `/system/espnow/peers/<mac>/identity.json`.
- During a button-initiated pairing window (Phase 2), peers exchange public keys, compute X25519 shared secret, derive per-direction AEAD keys via HKDF.
- Use the per-peer key in `esp_now_peer_info_t::lmk` instead of the passphrase-derived shared key. Existing peers without a per-peer key fall back to the passphrase key (compat).
- New `@PEER:<sig>:<command>` auth payload type: signature is ed25519(shared || msgId || cmdHash). Replaces `@BOND:<hmac>:<cmd>` for bonded-but-modern peers.
- USER_SYNC continues to work; it just rides on per-peer encrypted frames once a peer is bonded.

### Phase 2 — Bonding UX (2–3 days)
**Goal:** "Hold both devices' buttons for 5 seconds to bond" feels like BLE bonding.

- Add `BondingState { Idle, AdvertisingWindow, KeyExchange, Confirming, Bonded }` plus a 30 s timer.
- Wire to an existing hardware button (don't spawn a task per press; toggle a flag and let the ESPNOW task observe — see memory note about per-action tasks).
- LED indicator: blink during AdvertisingWindow, solid during KeyExchange, off when Bonded.
- Web `/bond` page gains a "Start pairing window" button mirroring the hardware button.
- During the window, the device sends a BONDING_HELLO broadcast every 1 s with its public key. Peers in the window respond with BONDING_HELLO back; both transition to KeyExchange.
- Capture a confirmation step (display the last 4 hex of the shared secret on each device's OLED, user verifies they match — defends against MITM in the open window).

### Phase 3 — RPC opcode and chunked response streaming (3–5 days)
**Goal:** Structured remote calls without abusing the CLI.

- New opcodes: `RPC_REQ`, `RPC_RESP_BEGIN`, `RPC_RESP_CHUNK`, `RPC_RESP_END`, `RPC_ERROR`.
- Payload: simple TLV — method id (2 bytes from a generated table), argc, [arg type, arg len, arg bytes]. Avoid msgpack/CBOR for now; we don't need the expressiveness and parser cost matters.
- Add an `EspNowRpcMethod` registry — each registered method declares `(uint16_t methodId, requiredRole, fnPtr)`. RPC payloads route through the same `executeCommand`-style auth/exec path so per-task TLS identity stays correct.
- Lift the existing CMD_RESP 2 KB cap by routing **all** remote CLI output through STREAM chunks during execution + a final small CMD_RESP carrying just the exit status. (Spec already exists; verify it covers the full response path and remove the 2 KB ceiling.)

### Phase 4 — Per-peer event subscriptions (2 days)
**Goal:** Replace "broadcast and hope" with explicit interest.

- New opcodes: `SUBSCRIBE { eventClassMask }`, `UNSUBSCRIBE`, `EVENT { class, payload }`.
- `EspNowState` gains a `Subscription[8]` table keyed by peer MAC + class mask.
- The event-emission code (where sensor thresholds, button presses, automations fire) iterates subscriptions and unicasts matching events — instead of broadcasting blindly.
- Wire the existing typed events (`sensor_status`, `threshold_crossed`, `button_press`, `automation_triggered`, `alarm_triggered`, `low_battery`, `system_error`, `wifi_*`, `ble_*`) onto event classes.

### Phase 5 — Concurrent file transfers (2 days)
**Goal:** Multiple peers can push/pull files at the same time without colliding.

- Replace `gFileTransferLocked` with `FileTransferCtx slots[N]` (N=4 is plenty; PSRAM buffer per slot).
- Slot key: (peer MAC, msgId). Allocator picks a free slot or returns BUSY.
- FILE_START sender gets back a slot id; subsequent FILE_DATA frames carry the slot id.
- Add a progress STREAM (small `FILE_PROGRESS` opcode with bytes-sent/bytes-total).

### Phase 6 — Mesh forwarding + routing table (3–5 days, optional)
**Goal:** A→C works when A can't see C directly.

- Confirm or implement decrement-and-rebroadcast for non-terminal mesh frames.
- Add a per-peer `lastHopVia[]` cache populated from observed traffic — proto-distance-vector. Don't aim for full DV; the topology discovery flow already collects enough to fill it.
- Add a `MESH_PATH_HINT` frame so peers can tell each other "I'm reachable via X" instead of relying on flood-and-dedup.
- Worth its own design doc if we go past two hops in practice.

---

## What to do first

If you only have a week, the highest-leverage sequence is:

1. **Phase 0** (foundation) — pays itself back immediately, no risk.
2. **Phase 3** (RPC + chunked response) — unblocks the most user-visible gap (truncated remote command output) and replaces the dead-ish ip-prefix gate with a real auth context model.
3. **Phase 2** (bonding UX) — makes the existing bond feature **feel** like a feature.

Phase 1 (per-peer crypto) is the most architecturally consequential and the most code; it's worth doing but only after the API surface above stabilizes. Phase 4 (subscriptions) is small but has a long tail of consumer wiring.

Phases 5 (concurrent files) and 6 (mesh) are independent — pick them up when they bite.

## Open questions / things to verify before coding

- **Multi-hop forwarding actually works** — write a 3-device test: A→B→C with A and C unable to hear each other. If it works today, document it; if not, scope Phase 6 properly.
- **STREAM frames during long CLI commands** — confirm the streaming path actually carries the full output, not just the head. If it does, the 2 KB CMD_RESP cap is cosmetic and Phase 3 is half-done.
- **Channel coexistence** — confirm Wi-Fi STA disconnects don't kill ESPNOW (they shouldn't — ESPNOW just needs a channel — but the radio coex notes in [`System_ESPNow.cpp:6812`](../components/hardwareone/System_ESPNow.cpp#L6812) hint at edge cases). Important for Bond mode if a bonded peer loses its AP.
- **PSRAM budget** — adding subscription tables + per-slot file buffers + per-peer identity records all live in PSRAM today. Confirm the budget; check sizeof reports from `size.json`.
- **Backwards compatibility window** — V3 is the wire format. Phases 1 and 3 add new opcodes; existing devices on old firmware will see them as `unknown type` and drop. Plan a one-release overlap where both `@BOND` (old) and `@PEER` (new) auth are accepted on the receive side.

## Non-goals (for clarity)

- **Replacing MQTT or HTTP.** ESPNOW is the right transport for offline, low-power, mesh-local scenarios. It's not a substitute for cloud connectivity.
- **OTA over ESPNOW.** Possible but explicitly out of scope here — the security and reliability story for OTA needs its own design doc.
- **General-purpose IP routing.** No tunneling TCP/UDP over ESPNOW.

---

## Appendix — files you'll touch most

- [components/hardwareone/System_ESPNow.h](../components/hardwareone/System_ESPNow.h) — opcode enum, payload structs, public API
- [components/hardwareone/System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp) — dispatch, handlers, bond state, sync, RPC
- [components/hardwareone/System_AuthIdentity.h](../components/hardwareone/System_AuthIdentity.h) — `ExecIdentityGuard`, `SYSTEM_IDENTITY_SCOPE`, `currentAuthContext()`
- [components/hardwareone/System_Utils.cpp:2922](../components/hardwareone/System_Utils.cpp#L2922) — where remote commands actually install their TLS identity (already correct)
- [components/hardwareone/System_Settings.h](../components/hardwareone/System_Settings.h) — `bondPeerMac`/`bondRole`/`bondModeEnabled`, `espnowPassphrase`
- [components/hardwareone/WebPage_Bond.cpp](../components/hardwareone/WebPage_Bond.cpp), [components/hardwareone/WebPage_ESPNow.{h,cpp}](../components/hardwareone/WebPage_ESPNow.cpp) — UX
- [components/hardwareone/BLE_Peers.h](../components/hardwareone/BLE_Peers.h) — the BLE pairing model to **not** confuse with bond mode; BLE peers are glasses/ring, bond is HardwareOne ↔ HardwareOne
