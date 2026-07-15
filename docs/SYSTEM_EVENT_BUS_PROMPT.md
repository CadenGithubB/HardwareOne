# System-Wide Event Bus — Architectural Exploration Prompt

Use this document as a self-contained brief for architectural design sessions
(larger models, design reviews, future implementation planning). It captures
context from the automation/ESP-NOW trigger conversation and the broader need
for a unified in-memory event layer.

Companion docs:
- [AUTOMATION_ESPNOW_TRIGGERS_PLAN.md](AUTOMATION_ESPNOW_TRIGGERS_PLAN.md) — poll vars (Tier A) + automation event triggers (Tier B)
- [AUTOMATION_TRIGGERS_EXPANSION_PLAN.md](AUTOMATION_TRIGGERS_EXPANSION_PLAN.md) — general system poll condition variables

---

## Project context

Designing firmware for **HardwareOne**, an ESP32-S3 embedded platform (ESP-IDF /
Arduino hybrid) with many subsystems: ESP-NOW mesh, bond mode (master/worker
paired devices), BLE (G2 glasses, bond peripherals), web server with SSE,
sensors, automations, OLED/G2 UI, file system, camera, speech recognition, etc.

The codebase is large (~15k lines in `System_ESPNow.cpp` alone, 90+
`logSystemEvent` call sites across 34 files). Build flags gate subsystems
(`ENABLE_ESPNOW`, `ENABLE_BONDED_MODE`, `ENABLE_BLUETOOTH`, `ENABLE_AUTOMATION`,
etc.).

---

## Problem we're solving

We want a **uniform way for subsystems to publish discrete events** when things
happen — not primarily a durable log, but an **in-memory event register /
shared queue** that multiple consumers can subscribe to:

- ESP-NOW mesh message received (heartbeat, text, file, sensor broadcast, boot notice, etc.)
- Bond device state change (online/offline, paired, sync complete, reject from unpaired sender)
- BLE device connect/disconnect (G2 glasses, bond peripheral, companion app)
- Web client session connect/disconnect
- Sensor lifecycle (online/offline, auto-disabled after failures)
- Automation-relevant edges (peer_online, peer_offline, text_rx, file_received)
- Potentially: settings changes, WiFi connect/disconnect, NTP sync, reboot reasons

**Key insight:** This is NOT the same as today's logging. We already have
fragmented event handling:

| Mechanism | Purpose | Limitation |
|---|---|---|
| `logSystemEvent(cat, fmt, ...)` | Durable `[EVENT][CAT]` lines → `/system/sys_logs/system-events.log` (256KB ring) | File I/O, low-volume by design, not actionable in real-time, not per-consumer |
| `blePushEvent()` / `broadcastEventToAllSessions()` | SSE push to logged-in web browsers | Web-only, BLE-specific JSON builder |
| `hijackFsmDispatch()` (G2) | FreeRTOS queue → FSM worker task | G2 glasses UI only, drop-on-full |
| ESP-NOW internal rings (`gEspNowRxRing`, text queue, stream queue) | Transport + subsystem dispatch | Not visible outside ESP-NOW; no unified schema |
| `g2ESPNowAppOnRxText()` | Kick G2 UI redraw on new text | UI-only, inline no-op when G2 disabled |
| `logSystemEvent("MESH", "peer ... online")` in `processMeshHeartbeats` | Durable audit trail | Logged but **never wakes automations** — zero `notifyAutomationScheduler()` calls from ESP-NOW |

We need something closer to an **event bus / register**: producers push compact
structured events; consumers (automation engine, web SSE, G2 UI, optional durable
log sink, debug trace) subscribe without each subsystem inventing its own queue.

---

## Prior conversation context (automation triggers)

We explored why ESP-NOW wire message codes should become automation triggers.
Current state:

1. **Schedule triggers** (time/interval/manual/boot) wake automations on a clock.
2. **Poll condition variables** (`bond_online`, `bond_paired`, `peers`, `bond_rssi`, etc.) are evaluated as gates when a schedule trigger fires. Bond "events" today are really **state polls**, not edge triggers.
3. **Gap:** Discrete events (peer X came online, text received, file arrived) cannot be expressed as poll conditions because:
   - Condition grammar is `VAR op VALUE` with no per-target argument (`PEERS>0` ≠ "BACKDOOR came online")
   - One-shot events have no lasting state to poll
   - Poll latency (30–60s interval) is unacceptable for immediate reactions

A narrow fix was proposed: automation-specific event ring + `espnowevent` trigger
type. But this is a symptom of a **missing system-wide event layer** —
automation would be one consumer, not the owner of the event infrastructure.

### Why bond mechanics don't generalize

Bond "triggers" are poll condition variables in `evaluateCondition()` — they
read current state (`isBondModeOnline()`, `BondedPeer::isPaired()`, etc.) when a
clock trigger fires. That works for bond because state is steady and singular (one
peer, one ONLINE/OFFLINE bool). It does not work for:

| Limit | Bond poll vars | ESP-NOW / system events |
|---|---|---|
| State vs edge | `BOND_ONLINE` is ongoing state | `peer_online`, `text_rx`, `file_received` are one-time edges |
| Per-target identity | One bond peer | Mesh has N peers; grammar has no argument slot |
| Wake latency | 30s poll acceptable | Immediate reaction needed |

Bond online/offline should stay as poll conditions. The bus handles per-peer
edges and discrete message events.

---

## User's request (the actual design question)

Design a **system-wide in-memory event register** where:

- When a mesh message appears → push to shared queue
- When a bond event occurs → same queue
- When BLE connects/disconnects → same queue
- When a web client connects → same queue
- Other subsystems follow the same producer API

### Requirements to consider

- **Embedded constraints:** limited RAM, no heap churn in hot paths, task-safe
  (producers on espnow_task, BLE callback, web server task; consumers on main
  loop or dedicated worker)
- **Drop policy:** bounded ring, drop-oldest or drop-newest on overflow (G2 FSM
  already does drop-on-full)
- **Not a log:** primary surface is in-memory for real-time consumers; durable
  `logSystemEvent` may become a *subscriber* that formats and persists a subset,
  rather than the canonical event path
- **Schema:** compact event envelope (source subsystem, event kind, timestamp,
  optional MAC/peer ID, small payload blob) that works across ESP-NOW opcodes,
  BLE connection slots, web session IDs, etc.
- **Consumer fan-out:** automation matcher, web SSE broadcaster, G2 page refresh
  kicks, debug trace, optional file log — each filters what it cares about
- **Relationship to existing code:** ~90 `logSystemEvent` sites, BLE_Events.h
  CompactJson+SSE, G2 hijackFsm queue, ESP-NOW v4 dispatch table with 40+
  opcodes — what gets unified vs. what stays subsystem-local?
- **Build flags:** stubs when subsystems disabled; events from disabled
  subsystems simply don't exist
- **Security:** distinguish authenticated events (bond paired, BLE secure channel)
  from attacker-controllable fields (beacon deviceName, unauthenticated mesh frames)

---

## Existing patterns worth referencing

```cpp
// Durable log (low volume, file-backed)
void logSystemEvent(const char* category, const char* fmt, ...);

// G2 FSM (task queue, drop on full)
void hijackFsmDispatch(HijackEvent ev, const char* tag);

// Automation wake (dirty flag only, no event payload today)
void notifyAutomationScheduler();

// ESP-NOW peer edge (logged, NOT fed to automation)
// processMeshHeartbeats ~7989: peer online/offline on heartbeat timeout
```

ESP-NOW V4 has 40+ wire opcodes (`System_ESPNow_Wire.h`): heartbeat(30),
boot(31), text(52), file_end(112), bond_heartbeat(170), sensor_broadcast(150),
etc. User-facing events should be **derived semantic events** (peer_online,
text_rx), not raw opcodes.

### Key source files

| Purpose | Path |
|---|---|
| Durable event log | `components/hardwareone/System_Debug.cpp` (`logSystemEvent`) |
| Log format docs | `docs/AUTH_LOG_FORMAT.md` |
| ESP-NOW wire opcodes | `components/hardwareone/System_ESPNow_Wire.h` |
| ESP-NOW RX dispatch + peer edges | `components/hardwareone/System_ESPNow.cpp` |
| Automation scheduler + poll conditions | `components/hardwareone/System_Automation.cpp` |
| BLE SSE events | `components/hardwareone/BLE_Events.h` |
| G2 FSM event queue | `components/hardwareone/G2_HijackFsm.h` |
| Automation ESP-NOW plan | `docs/AUTOMATION_ESPNOW_TRIGGERS_PLAN.md` |

---

## Questions for the architect model

1. **Bus vs. ring vs. pub/sub:** What pattern fits an ESP32 with 5–10 event sources and 3–5 consumers? Single MPMC ring? Per-source queues merged at drain? Topic-based subscription bitmask?

2. **Event schema:** Propose a concrete `SystemEvent` struct (size budget, variant payload, string interning or fixed fields). How do mesh MAC, BLE conn_id, web session_id, and bond role map into one envelope?

3. **Producer API:** What should `systemEventPost()` look like? Callable from ISR? From espnow_task? Synchronous vs. queued?

4. **Consumer API:** How do automations subscribe (`on: peer_online, match: BACKDOOR`)? How does web SSE filter? Polling drain vs. callback registration?

5. **Migration path:** Phase 1 MVP (which producers + which consumers)? Can automation event ring be built ON TOP of this bus rather than as a one-off? What happens to `logSystemEvent` — keep parallel, make it a subscriber, or deprecate gradually?

6. **Scope control:** This project is very large. What's the minimum viable bus that unblocks automation triggers without boiling the ocean? What's explicitly deferred?

7. **Comparison to existing embedded patterns:** ESP-IDF event loop, FreeRTOS event groups, Zephyr zbus, LwM2M — what translates to this constrained environment?

---

## Deliverables requested

1. Recommended architecture (with diagram)
2. Concrete `SystemEvent` schema and API signatures
3. Producer/consumer task-safety rules
4. Phased migration plan from today's fragmented logging
5. Explicit list of what NOT to unify (transport-internal ESP-NOW plumbing, per-iteration sensor samples, debug printf flood)
6. How automation `espnowevent` triggers become a thin consumer of this bus
7. Risks: RAM budget, flash wear if log subscriber writes every event, re-entrancy, event ordering across tasks

---

## Constraints

- Minimize scope — don't redesign the entire firmware
- Reuse existing conventions (FreeRTOS queues, `notifyAutomationScheduler`
  dirty-flag pattern, `#if ENABLE_*` guards)
- No Python scripts
- Poll conditions (`bond_online=OFFLINE`) remain valid for steady-state; the bus
  handles edges and one-shots
- Bond online/offline does NOT need bus events if poll vars suffice; bus is for
  per-peer and discrete message events

---

## Suggested phasing (starting hypothesis)

**Phase 0 — Design only:** `SystemEvent` schema, producer API, consumer drain
contract, RAM budget.

**Phase 1 — MVP bus + one consumer:**
- Producers: ESP-NOW peer_online/offline, text_rx, file_received
- Consumer: automation matcher (`espnowevent` triggers)
- Optional subscriber: debug trace (Serial, gated by flag)

**Phase 2 — More producers:**
- BLE connect/disconnect
- Web session connect/disconnect
- peer_paired (authenticated only)

**Phase 3 — More consumers:**
- Web SSE fan-out (replace per-module `blePushEvent` duplication?)
- G2 UI refresh kicks (replace `g2ESPNowAppOnRxText`-style one-offs)

**Phase 4 — Log rework (optional):**
- `logSystemEvent` becomes a filtered subscriber for audit-worthy events only
- Reduce direct `logSystemEvent` calls at producer sites over time

This phasing is a hypothesis — the architect model should validate or revise it.

---

## Relationship to automation Tier B plan

The automation-specific event ring proposed in the ESP-NOW triggers plan should be
implemented as a **consumer of this bus**, not a parallel one-off. If the bus
lands first, automation Tier B is thin matching logic; if automation ships first
as a narrow ring, plan to refactor it into a bus consumer rather than letting two
event systems coexist permanently.
