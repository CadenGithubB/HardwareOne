# ESP-NOW Automation Triggers - Findings & Plan

Status: Tier A1/A2 IMPLEMENTED (built green on feathers3, 2026-07-12) - awaiting HW
flash/test. Tier A3 (counters), A4 (presence lists), and Tier B (event triggers)
still deferred - see below.
Companion to docs/AUTOMATION_TRIGGERS_EXPANSION_PLAN.md (the 17 poll vars shipped
2026-07-12). This doc is specifically the ESP-NOW trigger surface.

## Implemented 2026-07-12 (13 vars, same evaluateCondition seam)
Enums: ESPNOW, BOND_MODE, BOND_ROLE, BOND_PAIRED, BOND_ONLINE, BOND_SYNCED, PAIRMODE.
Numeric: BOND_RSSI (gated on isBondModeOnline), BOND_PEER_HEAP, BOND_PEER_UPTIME
(both gated on bondPeerStatusValid), PAIRMODE_SECS, PEERSKNOWN, STALESTPEERAGE.
Added #include "System_BondedPeer.h"; gEspNow reads under #if ENABLE_ESPNOW,
BOND_PAIRED under #if ENABLE_BONDED_MODE, bond predicates use their header stubs.
New "ESP-NOW / Bond" optgroup in both web dropdowns + placeholders; USERGUIDE table
+ CLI help updated. NOT committed (HW test first).
DROPPED: BOND_TOKEN (isBondSessionTokenValid not header-exported - would need a
System_ESPNow.h change; add later if wanted).
STILL DEFERRED here: A3 counters (need edge/delta support = the flagged once-mode
task), A4 ONLINEPEERS/ONLINEROOMS (need the 32B->192B buffer bump), Tier B events.

## The state of things today (verified)

- Automations are **100% poll-driven**. Trigger types: TIME / MONTHLY / YEARLY /
  INTERVAL / MANUAL / BOOT (System_Automation.cpp, struct Trigger ~2240). nextFire
  is entirely clock-based; MANUAL/BOOT return 0 (armed externally).
- **ESP-NOW is already usable as an automation ACTION** - `espnowsend` /
  `espnowbroadcast` / `espnowsendfile` are CLI commands and automation command
  lists run arbitrary CLI. So an automation can already *transmit* over the mesh.
  **The entire gap is on the TRIGGER (input) side.**
- ESP-NOW <-> automation coupling today is only read-side: `PEERS` (live mesh
  count, the only remote-aware var) and self-identity `ROOM`/`ZONE`/`TAGS` (read
  THIS device's gSettings, NOT peers). Zero ESP-NOW events feed automations
  (no notifyAutomationScheduler calls anywhere in System_ESPNow*.cpp).

## Two structural limits that shape everything

1. **Condition grammar = single `VAR op VALUE`** (sensor[64]/op[16]/value[64],
   32-byte string compare buffer). Whole-mesh **aggregates** (PEERS>0) and
   **CONTAINS-against-a-synthesized-list** fit. **Per-target numerics** (a
   specific peer, a room's count) do NOT - no argument slot on the variable.
2. **No event engine.** Turning an ESP-NOW event into a trigger is net-new
   plumbing, not a wire-up. The safe wake seam exists though:
   `notifyAutomationScheduler()` (sets a dirty flag, no FS/lock, callable from any
   task) + the `cmd_automation_trigger` MANUAL-arm precedent (System_Automation.cpp
   ~1789-1877).

## Tier A - new POLL variables (fit the else-if seam just extended; low effort)

Pattern: copy MOTION/WIFI/BLE (enum -> currentStringValue) or PEERS (numeric ->
currentValue). Guard `#if ENABLE_ESPNOW` / `#if ENABLE_BONDED_MODE`; null-check
gEspNow. All accessors verified to exist.

### A1. Bond / pairing state (clean predicate accessors already written) - BEST value
| Var | Type | Accessor |
|---|---|---|
| `ESPNOW` | enum ACTIVE/NONE | `gEspNow && gEspNow->initialized` (top-level guard) |
| `BOND_MODE` | enum ACTIVE/NONE | `gSettings.bondModeEnabled` |
| `BOND_PAIRED` | enum PAIRED/NONE | `BondedPeer::isPaired()` |
| `BOND_ONLINE` | enum ONLINE/OFFLINE | `isBondModeOnline()` (System_ESPNow.cpp:12264) |
| `BOND_SYNCED` | enum SYNCED/ONLINE/OFFLINE | `isBondSynced()` (:12274) |
| `BOND_TOKEN` | enum VALID/NONE | `isBondSessionTokenValid()` (:816) - RCE session gate |
| `BOND_ROLE` | enum MASTER/WORKER | `isBondMaster()`/`bondRoleStr()` |
| `BOND_RSSI` | numeric dBm | `gEspNow->bondRssiLast` - REAL link RSSI from rx_ctrl (the good one); stale when offline, gate on BOND_ONLINE |
| `PAIRMODE` | enum ACTIVE/NONE | `espnowPairModeActive()` (:4197) - WPS window open |
| `PAIRMODE_SECS` | numeric | `espnowPairModeRemainingMs()/1000` |
| `BOND_PEER_HEAP` | numeric | `gEspNow->bondPeerStatus.freeHeap` (valid+fresh gated) |
| `BOND_PEER_UPTIME` | numeric | `bondPeerStatus.uptimeSec` (peer reboot detect) |

### A2. Mesh aggregates (grammar-legal, cheap array scans)
| Var | Type | Accessor |
|---|---|---|
| `PEERS` | numeric | ALREADY SHIPS (System_Automation.cpp:2958) |
| `PEERSKNOWN` | numeric | count gMeshPeers[i].isActive (known, not just alive) |
| `STALESTPEERAGE` | numeric sec | max(millis()-lastMeshHeartbeatMs) across peers |

### A3. Monotonic counters - CAVEAT: need edge/delta support to be useful
`ESPNOW_TXFAIL`, `ESPNOW_TXSENT`, `ESPNOW_STREAMDROPPED`, `BOND_UNPAIRED_REJECTS`
(security probe count + offender MAC), `ESPNOW_UNPAIRED_DEVICES`. All exist as
cheap uint32 reads (gEspNow->routerMetrics.* / streamDroppedCount /
bondUnpairedRejectCount / unpairedDeviceCount). **BUT** these only make sense on a
*delta/edge* ("increased since last check"), and the automation engine has no edge
tracking today (the dead `a_trigger_mode` "once" control - already flagged as a
task). An absolute rule like `ESPNOW_TXFAIL>5` would re-fire every poll forever.
=> Ship these only after once/edge support lands. Fixing once-mode unlocks them.

### A4. Per-peer / per-room presence via CONTAINS list - needs a buffer bump
`ONLINEPEERS` (CONTAINS <name>) and `ONLINEROOMS` (CONTAINS <room>) synthesize a
comma-joined uppercased list of alive peers/rooms from gMeshPeerMeta, so
`IF ONLINEPEERS CONTAINS BACKDOOR THEN ...` works WITHOUT a grammar change (name on
the RHS, like TAGS). **Blocker:** `currentStringValue` is only 32 bytes
(System_Automation.cpp:2751) - fits ~1-2 names. Enlarge to ~192-256B (also helps
TAGS/ROOM/ZONE). Note: gMeshPeerMeta is populated only on the MASTER.

## Not proposable / grammar-blocked
- **Peer BATTERY: does not exist on the wire.** V4PayloadHeartbeat has
  role/peerCount/rssi/reserved/uptimeSec/freeHeap/deviceName - only 1 spare
  `reserved` byte. Would need a wire-format change (the mesh-robustness "free HB
  byte") + a MeshPeerHealth.battery field; then only an aggregate MINPEERBATTERY is
  grammar-legal. Deferred.
- **Mesh peer RSSI (`WORSTPEERRSSI`): semantically weak** - the stored value is the
  remote's own WiFi-STA-to-AP RSSI (or -127 for ESP-NOW-only peers), NOT the link
  RSSI. Skip. (BOND_RSSI is the real-link-RSSI one, keep that.)
- **Per-target numerics** (`PEERSINROOM:kitchen>2`, `PEERAGE:backdoor>20`, per-peer
  heap): need BOTH a target arg AND a threshold; the one value slot can't carry
  both. Needs a grammar extension (`VAR:ARG op VALUE`, split sensor token on `:`) -
  touches the tokenizer shared by all ~40 vars. Defer until a concrete rule needs
  it. NOTE: per-target reactions are the natural home of EVENT triggers (match
  field in the trigger JSON), which sidestep the grammar entirely.

## Tier B - EVENT-driven triggers (net-new architecture; the real "ESP-NOW trigger")

What it requires: (1) `Trigger::Type::ESPNOW_EVENT` + a parseOneTrigger case reading
`{"type":"espnowevent","on":"peer_online|...","match":"<mac|name|room|*>"}`; (2)
nextFire returns 0 (externally armed like MANUAL); (3) an ESP-NOW-side source that,
on espnow_task, records minimal `{kind,mac}` into a small ring/flag and calls
`notifyAutomationScheduler()` - MUST NOT read FS or run commands inline (small
stack); (4) a matcher in schedulerTickMinute (main loop, FS-safe) that maps the
event to armed automations and fires condition-gated via queueAutomationSubCommand.
The trigger's `match` field carries the per-target argument, so this side does NOT
hit the condition-grammar wall.

Ranked event targets (hook points verified in System_ESPNow.cpp):
1. **peer_online / peer_offline** (MVP) - edge already isolated + debounced + logged
   at :7989 / :7995 in processMeshHeartbeats() on espnow_task (safe context, capture
   MAC before slot reclaim at :8000). Everything else becomes "+1 selector, +1 hook."
2. **peer_paired** (WPS auto-pair completed) - runDeferredPairModePair() ~:4262-4269
   on cmd_exec (safe stack). Onboarding/greet/auto-assign-room. Match on the
   AUTHENTICATED result, never the attacker-controllable beacon deviceName.
3. **bond_online / bond_offline** - :7949 (offline, safe sweep) / :2991 (online, RX
   path -> flag only). Mostly covered by polling BOND_ONLINE minus one tick.
4. **text_rx** (mesh as a command/notify bus) - highest ceiling, heaviest plumbing
   (capture sender+content), and the only one with a content-TRUST surface. Hook the
   text drain ~:8499 (where g2ESPNowAppOnRxText already kicks a task safely).
   Constrain matching (exact sender MAC + allowlisted tokens). Do last.
5. **file_received** (FILE_END) ~:4079-4109 - already deferred to cmd_exec, low risk.
6. **bond_reject** (unpaired sender probe) - security; mostly covered by delta-poll
   of BOND_UNPAIRED_REJECTS. Lowest priority.
OUT OF SCOPE: the bond/RCE CMD channel (v4h_cmd) - authenticated execution path,
keep separate from triggers.

## Recommended sequencing
1. **Now (cheap, high value):** Tier A1 bond/pairing enums + BOND_RSSI +
   BOND_PEER_HEAP/UPTIME + A2 aggregates (PEERSKNOWN, STALESTPEERAGE). All
   grammar-legal, edge-free, genuinely useful (e.g. `IF BOND_ONLINE=OFFLINE THEN
   ledcolor red`, `IF PAIRMODE=ACTIVE THEN print pairing open`). ~14 vars, same seam
   as the 17 just shipped.
2. **Small follow-ups (need one enabling change each):** A4 ONLINEPEERS/ONLINEROOMS
   (after the 32B->~192B buffer bump); A3 counters (after once/edge support).
3. **Separate project:** Tier B event triggers, starting with peer_online/offline.

## Open decisions
- Buffer: enlarge currentStringValue 32 -> ~192-256B (unblocks ONLINEPEERS/ROOMS +
  longer TAGS)? Any other size assumption in the compare path?
- Edge/delta support: implement once-mode (already a flagged task) to unlock counters?
- Master-only: document ONLINEROOMS/ONLINEPEERS as master-only, or teach workers to
  aggregate remote meta?
- Grammar extension `VAR:ARG` - build proactively or defer to first real need
  (event-trigger match field covers most per-target cases anyway)?
- Peer battery: spend the free heartbeat byte to transmit it (wire change)?
