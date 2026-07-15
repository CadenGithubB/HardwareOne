# Notification / Event-Register Integration - Plan

Status: PLAN, direction DECIDED 2026-07-13 - full replacement. The notify*()
layer gets deleted and the event register becomes the single notification
pipeline. Implementation waits until the current event-register work
(uncommitted) has passed HW testing; this cutover is the next change set
after that, as one clean break (no compat shims, per repo convention).

Companion docs:
- [SYSTEM_EVENT_BUS_PROMPT.md](SYSTEM_EVENT_BUS_PROMPT.md) - original architecture brief
- [AUTOMATION_ESPNOW_TRIGGERS_PLAN.md](AUTOMATION_ESPNOW_TRIGGERS_PLAN.md) - the trigger side, Tier B now implemented via the register

---

## 1. The notification system being replaced

Three stages, all in `components/hardwareone/System_Notifications.cpp`:

1. **Semantic entry points.** ~21 `notify*()` functions (`notifyWiFiConnected`,
   `notifyBatteryLow`, `notifySensorStarted`, ...) called at the moment of the
   event from whatever task detected it. 7 of them are dead (zero callers).

2. **Fan-out, hardcoded per function.** Up to three sinks:
   - **OLED transient banner/ribbon** - `oledNotificationBannerShow()` /
     `oledPairingRibbonShow()` (OLED_UI.cpp). Global struct write; main loop
     renders/expires it.
   - **Persistent OLED queue** - `oledNotificationAdd()` (OLED_Utils.cpp),
     8-slot ring (message/subsource/level/source/timestamp/read). The bell +
     unread count in the OLED header. OLED-only; invisible to web/CLI.
   - **Web toast** - static `notifyWeb()` -> SSE `notification` event ->
     hw.notify browser toast.
- **Attribution:** per-task TLS (source enum + subsource string) via
   `NotificationContextGuard`; consumed only by the OLED queue.
- **Throttling:** hardcoded 30s cooldown statics for battery/USB/login-fail.

Already wired (uncommitted): every live notify*() posts its event twin, and
`systemEventsNotifyTick()` renders mesh/bond/BLE events as banner+toast from
the main loop. The cutover generalizes that renderer into the ONLY path.

## 2. Decisions (made 2026-07-13)

1. **Attribution: keep FULL fidelity, promote it into the event schema.**
   `SystemEvent` gains `uint8_t source` (interface: web/cli/oled/g2/voice/
   remote/system) AND `char who[16]` (username / device / IP prefix).
   `systemEventPost` stamps both from the caller's TLS notification context
   by default, with optional explicit-override parameters. The
   `NotificationSource` enum MOVES from System_Notifications.h into
   System_Events.h (events is now the base layer; notifications include it).
   Source+who become part of the whole event surface:
   - `events` CLI output shows them (`... setting_changed ledBrightness (5) by web:hub`)
   - automation `match` extends to substring-test who as well as
     subject/detail (`on=setting_changed match=hub` = only hub's changes)
   - the notification-center view and any future SSE feed carry them.

2. **Ring 24 -> 48 slots, and the OLED queue array is DELETED.**
   sizeof(SystemEvent) goes 92 -> 108 with source+who. Two static buffers
   exist (the ring + the automation drain buffer), so:
   - today: 24 x 92 x 2 = ~4.4 KB internal DRAM
   - after: 48 x 108 x 2 = ~10.4 KB internal DRAM (net +6 KB)
   - reclaimed: the 8-slot OLEDNotification array (~0.7 KB) - the persistent
     queue VIEW derives from the ring instead (filter queue-enabled kinds),
     with one `lastReadSeq` watermark replacing per-entry read flags.
     History depth effectively grows 8 -> 48 and becomes web-readable.
   - net ~ +5.3 KB. Verify with `memreport` after the current pile is
     flashed; fallback to 32 slots (net +2.5 KB) if DRAM looks tight.
   - cmd_events' stack copy grows to ~5.2 KB on cmd_exec (32 KB stack) - ok,
     but re-check its headroom at cutover.

3. **Boot-time events:** renderer cursor starts at 0 (not "from now").
   The 10s staleness gate applies to TRANSIENT sinks only (banner/toast);
   the queue view has no staleness cut, so boot-time sensor starts appear in
   the notification center like they do today.

4. **Two oddballs are UI feedback, not notifications - reclassify:**
   - `notifyVolumeChanged` -> direct banner call at the FM-radio site.
   - `notifyRemoteCommandReceived`'s "Running: X" banner (+ its dead
     in-place-update partner) -> direct banner at the espnowremote site.
   Neither gets an event kind; they never enter the pipeline.

## 3. Target architecture

```
subsystem code ──systemEventPost(kind, subject, detail [,source,who])──▶ 24..48-slot ring
                                                                            │ cursors
                 ┌──────────────────────────────────────────────────────────┤
                 ▼                          ▼                               ▼
        automation matcher          notification renderer            events CLI / future SSE
        (schedulerTickMinute)       (main loop, rules table)         (inspection)
                                    ├─ OLED banner (transient)
                                    ├─ queue VIEW over ring (persistent, lastReadSeq)
                                    └─ web toast via SSE
```

The rules table (one row per kind, compiled defaults, settings overrides):

```
kind -> { sinks: BANNER|QUEUE|TOAST[|G2|LED later], level, durationMs,
          cooldownMs, format template }
```

- Renderer drains ALL pending events per loop pass (not 4), applies per-kind
  cooldowns from the table (replacing the scattered statics - battery/USB/
  login throttles become table rows), formats text from per-kind templates
  (replacing ~21 hand-rolled snprintf bodies).
- Producers never render UI; posting is the entire producer API.
- Lossiness note: notifications become drop-oldest through the ring. Only
  chatty kinds (buttons/gestures) can realistically burst 48 deep between
  ~12ms loop passes, and those don't toast. Accepted.
- Latency note: render moves to the next loop pass (<= ~12ms). Imperceptible.

## 4. What gets deleted / simplified

- The 7 dead notify functions - deleted outright.
- The other ~14 notify bodies - deleted; their ~20 call sites post events
  directly (most already post the twin; two calls collapse to one).
- `notifyWeb()` - folded into the renderer.
- Cooldown statics - replaced by table rows.
- `oledNotificationAdd` + the 8-slot array + per-entry read flags - replaced
  by the ring-backed queue view + `lastReadSeq`.
- `NotificationContextGuard` / TLS context SURVIVES (it is how posts learn
  source+who) but moves conceptually into the events layer.
- Net: a deletion. System_Notifications.cpp shrinks to renderer + rules
  table + TLS context; flash and a little RAM come back.

## 5. Notification settings (unlocked by the rules table)

- The table IS the settings surface: per-kind sink mask + cooldown override.
- Grouped UI (Mesh, Connectivity, Power, Storage, Sensors, Security, Media,
  System) with per-category defaults; per-kind overrides on demand.
- Storage: one settings section, per-kind byte mask; absent = compiled default.
- CLI: REAL per-setting commands (no auto-registration), routed through
  handleSettingCommand so setting_changed fires like any setting write.
- Defaults matter more than the UI: most kinds default to event-only.
- Security guardrail: `login_fail`, `bond_reject`, `user_request`,
  `settings_save_failed` are un-silenceable (or default-on + warning).

## 6. Phasing

**Phase 0 - HW-test the current pile** (event register, triggers, producers,
review fixes). Nothing here starts until that passes; debugging a missing
toast must not mean bisecting two architectures at once.

**Phase 1 - the cutover (one breaking change set):**
schema (source+who, ring 48, enum move) -> rules/format table with compiled
defaults matching today's behavior -> renderer generalized -> call sites
switched -> notify bodies + queue array deleted -> oddballs reclassified.
Verification checklist: same toasts for same actions, queue entries carry
source+who, cooldown behavior preserved, no double-renders, `events` shows
attribution, automation match-on-who works.

**Phase 2 - settings** (section 5) + web notification-center panel reading
the ring (the visible payoff).

**Phase 3 - new sinks:** G2 allowlist (worker-path render, hijack-aware,
per-kind opt-in), LED patterns (e.g. user_request pulses until pendinglist).

## 7. Explicitly NOT doing

- Replacing `logSystemEvent` (durable audit file) or `broadcastOutput`
  (console pipe) - different contracts, analyzed and rejected earlier.
- Sound/buzzer sink - no driver exists.
- BLE GATT event characteristic changes - untouched by this plan.

## 8. Remaining open items

1. who[] length: 16 bytes covers usernames/device names; IPs truncate to a
   prefix. Acceptable? (Widening to 24 costs ~+1.2 KB across both buffers
   at 48 slots.)
2. Whether `events` grows a `read`/`clear` subcommand tied to lastReadSeq,
   or the watermark stays UI-only (OLED bell + web center).
3. G2 allowlist defaults (Phase 3): which kinds are glanceable-useful.
4. Exact un-silenceable list.
