# Accessory presence (case / charging / worn) as a Bluetooth power lever

Plan written 2026-10-28 on branch `codex/android-ble-relay`, checkout
`aa6508fd` plus local changes. Nothing in firmware changes with this document.
The host-side artefacts it names were written and run; the firmware steps are
proposals.

## Goal

Make the ESP32 know when the G2 glasses are in their case (or charging, or
simply set down) and when the R1 ring is off the finger, and use that
knowledge to spend less battery on the Bluetooth links to them overnight,
without ever fighting the user's explicit open/close.

Done means all four of these hold:

1. **The idea is a written plan** (this file), with the signals, the state
   machine, the actions, the firmware touch points and the measurement that
   decides whether it was worth it.
2. **It has been double-checked.** Section 7 is an adversarial pass over the
   plan's own assumptions; each finding either changed the plan or is carried
   as an open risk.
3. **Code examples exist outside the firmware.** `tools/g2/g2_case_decoder.py`
   decodes the two frame families on the host and is the oracle the firmware
   parser must agree with. `tools/g2/g2_presence_policy_sim.py` is the state
   machine as runnable Python with unit tests and a nightstand
   scenario (twenty-one tests after the review pass and the in-case-only
   decision of 2026-10-29). `docs/g2_proto/glasses_case.proto`
   records the schema (that folder is git-excluded locally, like the rest of
   the RE material).
4. **Downstream effects are considered.** Section 8 lists every subsystem the
   change touches, with what it must keep doing.

The measurable outcome at the end of the firmware work is a battery-log
comparison (section 6): the same overnight scenario with and without the
policy, reading `/battery.csv`, with the glasses and ring batteries read back
in the morning so the saving is not just moved onto the accessories.

## 1. How the Even app knows (what the evidence actually says)

The official Even app is closed. What we have is the community
reverse-engineering (`docs/FlutterApp-main`, generated from real protobuf
descriptors) and our own captures. Together they give three signals.

| Signal | Where it lives | Status |
|---|---|---|
| `chargingStatus` (field 13) | sid 0x09 settings reply. The glasses answer a BASIC read by echoing the whole `DeviceReceiveRequestFromAPP` back in **outer field 4** with every setting populated (observed; `System_G2_Protocol.cpp` notes that outer field 6 `Device_Respond_To_App` is a two-field ack the firmware never emits for reads, and `G2_PROTOCOL.md` line ~763 still says "field 6": stale). | Field number is wire data. The community app stores it in its settings snapshot but never acts on it. Our firmware already parses this echo for battery (12) and versions (5/6); field 13 is skipped. **Cheapest signal to add: a few lines in the existing parser.** |
| `GlassesCaseInfo` (sid 0x81) | `GlassesCaseDataPackage{commandId=CASE_INFO, magicRandom, caseInfo{soc, chargeStatus, lidStatus, inCaseStatus, errorCode}}` | Schema is wire data. Our firmware has **observed** these frames glasses→host when the case is nearby (`08 01 10 <magic> 1A 06 08 0C 10 01`). The community app never consumes `caseInfo`, so nobody has corroborated what the fields mean. The recorded byte shape is internally inconsistent (inner length says 6, only 4 inner bytes follow) because the firmware's RX frame ring keeps only the first 12 payload bytes per frame (`head[12]`, printed by `g2RingDump`); the last two inner bytes were cut off and may be the lid/inCase fields. |
| `deviceRunningStatus` (field 11) and `wearDetectionSwitchRestored` (field 10) | same settings reply | Running status is unmapped. Wear-detection switch is the glasses' own on/off for their wear sensor, already written by `g2glasses weardetect` (see `G2_WEAR_DETECTION_23214.md`); it is a setting, not a state. |

And one signal the glasses never send but we already infer:

| Proxy | Mechanism | Status |
|---|---|---|
| `g2_plugin_alive` / `g2_plugin_silent` events | When the glasses leave the head their EvenCore plugin task goes dormant and stops answering our 5 s renderer heartbeat; the firmware latches that as NOT_WORN and flips back on the first strict ACK. | Shipped and HW-validated. Latency is several missed heartbeat intervals. It does not distinguish "on the desk" from "in the case". |

The ring is simpler: `R1_SUB_WEAR_STATUS` arrives as a typed sample
(1 = notWear, 2 = wear) and already posts `ring_worn` / `ring_not_worn`.

**Conclusion on "how the app does it":** the glasses push case telemetry on
sid 0x81 and charging state on sid 0x09. The community app decodes both but
reacts to neither; whatever the official app does with them is not visible to
us. We do not need the app's behaviour, only the field meanings, and those
come from a labelled capture (section 3).

## 2. What the firmware does today that this builds on

- **Settings RX path** `G2_Glasses.cpp` ~12002: `g2SettingsReceive` (typed
  settings transactions) then battery, silent-mode and version parsers over
  the same payload. A charging parser slots in beside the battery one.
- **Unknown sids:** the RX envelope is logged (seq/sid/flag/pbLen) under
  DEBUG_G2 and the frame ring (`g2RingDump`) keeps the **first 12 payload
  bytes** of each frame. A `GlassesCaseDataPackage` with all four caseInfo
  fields is 13 to 15 bytes, so the tail is lost. The capture step needs the
  whole payload for sids with no parser: widen the head, or add a full hex
  dump under `debugg2protocol` for unparsed sids.
- **Connection-interval arbiter** (~20635): the glasses run their own
  two-state link power machine (active 30 ms / idle 105 ms with slave latency
  4). Our policy is to assert a fast interval only while something needs
  throughput and otherwise assert nothing. Measured 2026-07-27: the ESP32 side
  barely notices interval changes; the glasses do. **So widening the interval
  further is not an ESP32 battery lever.** What costs us is (a) our own
  periodic traffic and (b) the C6 keeping links alive at all.
- **Our own periodic traffic per temple:** EvenCore heartbeat every 5 s, base
  heartbeat every 15 s, battery/status pushes, notification renders. Each one
  wakes both radios.
- **Peer registry** (`BLE_Peers.h`): per-peer auto-reconnect with paced
  admission, `blePeerNoteUserDisconnect` (user close quiets reseek for the
  session), `blePeerRequestReseek`, `bleAutoReconnectTick`. Parking and
  reseeking already have the primitives.
- **One-temple repair** (`G2_RepairCore.h`): a half-connected pair retries the
  missing temple on a 7-step fast schedule then every 10 min, restarted by a
  wear edge or gesture. A deliberate park must not look like a half-connected
  pair to it.
- **Link budget:** the live C6 reported 3 BLE links tonight (`openring`
  refused with "this radio allows 3 BLE links"), while the companion build in
  the tree is configured for 4 (`CONFIG_BT_LE_MAX_CONNECTIONS 4`,
  `PHONE_PIPE_HANDOFF.md` says the four-link image went on 2026-10-18). So the
  bench C6 is running an older image, or its link report is wrong; settle
  which before counting on 4. Either way phone pipe + two temples filled it,
  and parking the glasses frees two links.
- **Power policy** (`System_Power`, `CROSS_BOARD_POWER_POLICY.md`): services
  publish busy flags; the policy reads them. Connected light sleep is
  explicitly not qualified. This plan stays inside that: it changes what
  traffic and links exist, not sleep modes.
- **Battery logging:** `/battery.csv` every 60 s by default, already on.

## 3. Phase 0: labelled capture (the blocking step)

Everything after this depends on knowing what the bytes mean. One evening
with the bench P4 and the glasses.

**Firmware prerequisite (small, standalone, build-gated):** under DEBUG_G2,
dump the whole protobuf payload as hex for any RX sid that has no parser
(the frame ring's 12-byte head is what truncated the only 0x81 frame on
record), and for sid 0x09 frames carrying field 13 or 11. One `DEBUG_G2F`
per frame, capped at 64 bytes. No behaviour change.

**Scenario script** (hand-driven, each step announced on the serial console
with a `log mark <label>` style line so the capture is self-labelling):

| # | Step | Expect to learn |
|---|---|---|
| 1 | Glasses on head, both temples up, 2 min idle | baseline: no 0x81; settings reply `chargingStatus=0` |
| 2 | Take glasses off, hold in hand 2 min | `g2_plugin_silent` latency; still no 0x81 |
| 3 | Put on desk 5 min | nothing new, confirms desk == hand |
| 4 | Bring closed case near (glasses outside) 2 min | whether 0x81 appears from proximity alone, `soc` meaning (case battery?) |
| 5 | Open lid, glasses still outside, 1 min | `lidStatus` polarity |
| 6 | Seat glasses in open case 1 min | `inCaseStatus` polarity, `chargeStatus` |
| 7 | Close lid 5 min | whether links survive, whether frames keep coming, `chargingStatus` in settings reply |
| 8 | Open lid, remove glasses, put on head | the recovery edge: which arrives first, 0x81 / settings / heartbeat ACK |
| 9 | Charge via cable without case (if the hardware allows) | separates "charging" from "in case" |

Request a BASIC settings read (`g2glasses` read path) at each step so field
13 is sampled, not just pushed.

**Decode on the host:** `python3 tools/g2/g2_case_decoder.py --log
<capture>` once the dump lines exist, or paste hex directly. The decoder
marks presence-relevant fields with `*`, prints unknown fields rather than
dropping them, and prints `absent(=0?)` for omitted proto3 scalars so the
elision ambiguity is visible in every line.

**Exit criteria:** a table of (step → fields seen → values), with polarity for
`lidStatus` and `inCaseStatus` stated or marked undetermined. Update
`docs/g2_proto/glasses_case.proto` comments and the sid table in
`G2_PROTOCOL.md`.

## 4. Phase 1: decode and surface (firmware, no policy yet)

1. `g2ParseSettingCharging` beside `g2ParseSettingBattery`: field 13 from
   the outer-field-4 echo, same walk as the battery parser; mirror as
   `gChargingR/L` (-1 unknown), push a
   `charge-L/R` status event on change only, post `SYSEVT_G2_CHARGING` /
   `SYSEVT_G2_DISCHARGING` (new catalog rows, family GLASSES).
2. `g2ParseCaseInfo` for sid 0x81 in `System_G2_Protocol.cpp`, validated
   whole-payload like the other parsers (a malformed suffix never yields a
   value). Mirror `gCase{soc, charge, lid, inCase, rxMs}`; post
   `SYSEVT_G2_CASE_LID` (subject open|closed) and `SYSEVT_G2_IN_CASE`
   (subject in|out) on edges once polarity is known from phase 0.
3. Surface on `g2status` / `g2info` (`charge=`, `case=`), the web
   `g2-status` SSE, and the G2/OLED Status pages that already show battery.
4. Host test: feed the decoder's self-test vectors through the C++ parser in
   a host harness (the repo already runs `G2_RepairCore.h` and settings codecs
   on the host). The Python decoder is the oracle.

Nothing in phase 1 changes link behaviour. It is useful on its own: the
status pages stop saying "connected" for glasses that are in a box.

## 5. Phase 2: the presence policy

The state machine is `tools/g2/g2_presence_policy_sim.py`, verbatim. Per
peer (glasses pair, ring):

```
UNKNOWN ──worn──▶ WORN ──not worn──▶ SET_DOWN ──in case──▶ IN_CASE
   ▲                                   │ quiet after 2 min     │ quiet at once
   │                                   │ (at once if charging) │ park after 5 min
 link up                               │ NEVER parks           ▼
   └─────────────────────────────────────────────────────── PARKED
                                                              │ reseek: 10 min, doubling to 60 min
link lost from WORN / SET_DOWN / UNKNOWN ──▶ UNKNOWN + RECONNECT_FAST
   (the registry's aggressive auto-reconnect and one-temple repair, as today)
link lost from IN_CASE ──▶ PARKED (slow back-off)
USER_CLOSED: user said close. Policy observes nothing, does nothing, until open.
```

**The in-case rule (user decision, 2026-10-29).** Only a peer that has been
marked *in case* (sid 0x81 `inCaseStatus`, or the ring's dock report) may be
parked, and only a park arms the slow, backing-off reseek. Everywhere else
the firmware stays as aggressive as it is today: glasses set down on a desk
or charging on a cable keep their link (QUIET only), and if that link drops
the registry's fast auto-reconnect runs. The reason is the asymmetry of the
two mistakes: a slow reseek on glasses the user is about to pick up costs a
visible reconnect delay every morning, while a fast reseek on glasses that
are really in the case costs some battery on one night. The cheap mistake is
the one the policy is allowed to make. Charging alone is not the case: a
cable while worn is a real use, and a cable on the desk is just set down.

Actions, in order of how much they save and how little they risk:

| Action | What it does | Saves | Risk |
|---|---|---|---|
| **QUIET** | Slow the EvenCore heartbeat from 5 s to 60 s (never stop it: its strict ACK is the only worn edge we have), stop the 15 s base heartbeat, status pushes and notification renders to that peer. Link stays up; the glasses' own idle machine runs. | Our share of radio wakeups on P4+C6. Modest. | The renderer watchdog counts missed intervals; at 60 s cadence its silence latch must count intervals, not seconds, or be told the cadence, so it neither escalates early nor re-fires `g2_plugin_silent`. |
| **PARK** | Disconnect the links on purpose through the peer registry with a new `blePeerNotePolicyPark(kind)` that, unlike a user close, arms a slow reseek. Both temples at once. Reached **only from IN_CASE**: after 5 min seated, or at once if the in-case glasses drop the link themselves. | Two of three C6 links idle; the C6 can sleep deeper (its build has `CONFIG_BT_LE_SLEEP_ENABLE`); no supervision traffic. The largest lever available without connected light sleep. | Reconnect costs a scan. Lose the push signals (0x81, settings) while parked, so recovery relies on reseek cadence or sibling evidence. Must not trigger one-temple repair. |
| **RESEEK_NOW** | Scan for the parked peer immediately. | — | Scanning is the most expensive radio state; this is why the cadence backs off (10, 20, 40, 60 min cap) and why a sibling worn edge (ring goes on the finger, other temple ACKs) jumps the queue. The first simulator run with a flat 10 min cadence produced 54 scans a night; the back-off makes it under a dozen. The back-off exists only after an in-case park. |
| **RECONNECT_FAST** | The link dropped while the peer was not marked in case. The policy arms nothing and steps aside: the registry's auto-reconnect and the one-temple repair run exactly as today. | — | None new. This is the deliberate "stay aggressive" branch; it is the existing behaviour, named so the firmware port and its tests can assert it. |

Signals that drive it, with their source and latency:

| Edge | Source | Latency |
|---|---|---|
| worn / not worn (glasses) | existing heartbeat latch | ~3 heartbeat intervals (15 s normal, 3 min while QUIET) |
| link lost while IN_CASE | peer registry link-lost note, routed to the policy | immediate; treated as PARK so the slow back-off applies instead of the registry's fast auto-reconnect |
| link lost while WORN / SET_DOWN / UNKNOWN | same note | immediate; RECONNECT_FAST, the registry's existing behaviour, no policy schedule |
| charging / discharging (glasses) | phase 1 field 13 | next settings read (poll it every 60 s while SET_DOWN; today it is read on demand); QUIET evidence only, never a park reason |
| in case / out of case (glasses) | phase 1 sid 0x81 `inCaseStatus` | push, immediate, only while linked; **the only park evidence**; in-case outranks a stale worn latch, a fresh worn edge wins straight back |
| worn / not worn (ring) | existing `R1_SUB_WEAR_STATUS` | per sample |
| ring charging | ring battery frame `not-charging` text already exists (`Bat=55% not-charging`) | per sample |
| user open/close | `openg2/closeg2/openring/closering` | immediate, always wins |

Settings (persisted, with per-peer rows on the existing Bluetooth pages):

- `blePresencePolicy` on|off (default off until phase 3 numbers exist)
- `blePresenceQuietAfterMin` (2), `blePresenceCaseParkAfterMin` (5),
  `blePresenceReseekMin` (10), `blePresenceReseekMaxMin` (60); there is no
  set-down park timer because set-down never parks
- `blePresenceParkMinBatteryPct` (0 = ignore) so a nearly flat accessory is
  not parked away from the status page that would tell you about it; an
  unknown battery counts as too low while the floor is set

Ring specifics: the ring is worn overnight in the user's own scenario, so the
ring's policy mostly stays WORN. When it is off the finger and charging, the
same machine applies. The ring's own low-power control (`ring control
lowpower`) is a separate, already-built lever and reports
`feature-unsupported` on the 2.3.2.0008 profile; do not conflate the two.

## 6. Phase 3: measure, then decide the default

`P4_BATTERY_BUDGET.md` is explicit that no current instrument exists and the
`voltage` command is unsuitable. The honest comparison is the one the device
already produces:

1. Same hardware, same overnight window (unplugged, screen blanked, Adaptive
   mode, health logging on, ring worn), three nights each:
   - A: policy off, glasses in closed case, links held (tonight's setup)
   - B: policy on
   - C: control: glasses closed by the user (`closeg2`) before bed
2. Read `/battery.csv` start/end voltage and slope; read `g2status` battery
   and `ring health` battery in the morning. If B saves less than C by a
   margin that matters, the saving is the links and QUIET is noise; if B is
   close to C, ship B as default-on for charging only.
3. Record the three slopes in this document's "Results" section before any
   default changes.

## 7. Double-check: adversarial pass over the plan

Each item was raised against the plan as first drafted. "Changed" means the
plan above already reflects it.

| # | Challenge | Verdict | Effect |
|---|---|---|---|
| 1 | "Widen the connection interval while in the case." | **Rejected.** The arbiter's measured note says the glasses already drop to 105 ms with latency 4 on their own and the ESP32 side barely notices interval changes. The cost we control is our traffic and the existence of the links. | Changed: QUIET and PARK replace an interval tier. |
| 2 | sid 0x81 frames only appear "when the case is around". Is the glasses relaying a case radio, or is it the glasses' own charger contact? | **Unknown, and it matters.** If it is proximity, `inCaseStatus` may be the only honest "seated" bit and `soc` is the case battery. If it is contact-only, no frame means out of case and silence is informative. | Phase 0 steps 4–6 separate these. The policy must not treat "no 0x81" as evidence either way until then. |
| 3 | proto3 omits zero scalars, so `inCaseStatus=0` and "field absent" are identical bytes. | **Confirmed.** `G2_WEAR_DETECTION_23214.md` hit the same trap on the wear switch. | Decoder prints `absent(=0?)`; the firmware parser must read the whole correlated frame and interpret absent as 0 only within a frame whose other fields prove the schema. |
| 4 | The recorded 0x81 shape has length 6 but four inner bytes. | **Confirmed inconsistency.** The transcription dropped two bytes. | Phase 0 prerequisite: dump every payload byte; the decoder self-test uses the consistent 4-byte form and says why. |
| 5 | PARK looks exactly like a half-connected pair or a link loss to the one-temple repair and to `blePeerNoteLinkLost`. | **Real.** Repair would start scanning on its 7-step fast schedule, defeating the park. | Phase 2 names a distinct `blePeerNotePolicyPark` intent, observed by repair the same way it observes a user close ("held"). Test in the repair host test. |
| 6 | QUIET starves the renderer heartbeat; the watchdog declares the plugin dead, posts `g2_plugin_silent` again and the status page shows "plugin silent". Worse (independent review): if QUIET **stops** the heartbeat it also removes the only worn edge, so the glasses picked up off a cable never leave the off-body state (CHARGING in the first machine; SET_DOWN/IN_CASE now) and could get parked. | **Real, and the first draft had it wrong.** | Changed: QUIET slows the heartbeat to 60 s and never stops it; the watchdog counts intervals at the active cadence; the simulator has a test that a worn edge still lands while quiet. |
| 7 | While PARKED we cannot receive the lid-open event that should wake us. | **Real, by construction.** | Recovery is the back-off reseek plus sibling edges (ring worn, the user touching the device, local input). A gesture on the glasses cannot reach us while parked; accept a worst-case 60 min reconnect while charging, or set the ceiling lower in settings. Phase 3 decides. |
| 8 | A flat reseek cadence could cost more than the links it replaced. | **Confirmed by the simulator** (54 scans/night at 10 min). | Changed: doubling back-off to a 60 min ceiling, under a dozen scans a night; sibling edges reset it. |
| 9 | Parking frees C6 links and the phone pipe's boot-order admission could grab them, then the glasses cannot come back. | **Real tonight** (phone auto-reconnect was turned off by hand for this reason). The live C6 reports 3 links while the tree's companion build says 4; that discrepancy is its own open item. | Reseek must reserve link slots for parked peers, or admission must rank a parked peer above an idle pipe. Put in the peer registry, not in the policy. |
| 10 | The user toggled a state (`openg2`) and the policy un-toggles it overnight. Memory rule: never auto-stop user-toggled state after a lens action. | **Policy violates the letter.** | The setting is default-off until measured; a park is logged as an event and shown as "parked by policy" on the status pages, never as "disconnected"; a user open always wins and sets USER state for the session. |
| 11 | Charging while worn (someone uses the glasses on a cable) would be CHARGING → QUIET → PARK in the first machine. | **Edge case**, mostly moot since item 20: charging alone never parks. | WORN outranks CHARGING: a charging edge while WORN stays WORN (`test_charging_while_worn_stays_worn`). The charge-before-worn order (UNKNOWN → CHARGING with the glasses actually on) is covered by item 6's slow heartbeat: the worn edge still arrives. |
| 12 | Does this save anything on the P4 at all, given the C6 does the radio? | **Unknown.** The budget document says the C6's connected cost cannot be counted during connected use and that no instrument exists. | Phase 3 is the gate; nothing becomes default before it. Also the reason PARK (fewer links, C6 can sleep) is preferred over anything that keeps links alive. |
| 13 | Settings reads for field 13 need request/response correlation and the firmware dedups by magic (2.3.2.14 finding). | **Confirmed from `G2_WEAR_DETECTION_23214.md`.** | Use the typed settings path with fresh magic, not a raw probe; `g2probe` already blocks sid 0x09. |
| 14 | The ring part is speculative; tonight the ring is worn, so it never parks. | **Accepted.** | Ring policy is the same machine with the same settings; its value is for the daytime "ring on the charger" case, and it is not on the critical path. |

| 15 | (independent review) The plan said field 13 lives in `deviceRespondToApp` (outer field 6). | **Wrong.** The glasses echo the request in outer field 4; field 6 is a two-field ack never sent for reads. `G2_PROTOCOL.md` carries the same stale line. | Changed throughout; decoder maps f4 as `echo.*` and f6 as `ack.*`, self-test vector moved to f4. Fix the protocol doc line when phase 0 updates it. |
| 16 | (independent review) The decoder checked the CRC over header+payload and its self-test generated the CRC the same wrong way. | **Wrong.** Both the protocol doc and `g2BuildEnvelope` compute it over the payload only. | Changed; any real envelope would have printed `crc=BAD`. |
| 17 | (independent review) "Payload bytes are not logged" and "transcription dropped two bytes". | **Wrong cause, right symptom.** The frame ring keeps a 12-byte head; the frame was truncated by the ring, not by a human. | Changed; phase 0 prerequisite is "widen or bypass the head for unparsed sids", smaller than a new dump. |
| 18 | (independent review) Link lost while SET_DOWN/CHARGING went to UNKNOWN, so the registry's fast auto-reconnect ran all night in exactly the target scenario if the glasses drop the link when the lid closes. | **Real.** | Changed: a loss while IN_CASE is a PARK with back-off (`test_link_lost_while_in_case_parks_with_backoff`). Narrowed by item 20: a loss while merely SET_DOWN or charging on a cable stays fast on purpose. Phase 0 step 7 tells us whether the lid actually drops the link. |
| 19 | (independent review) `park_min_battery_pct` was bypassed when battery was unknown; `on_user_open` reseeked on a live link; `on_user_close` left `link_up` stale. | **Real, simulator bugs.** | Fixed with tests. |
| 20 | (user, 2026-10-29) The first machine parked glasses that were only set down (after 20 min) and treated any off-body link loss as a slow reseek, so glasses on a desk could face an hour-long reconnect in the morning. | **Accepted as a requirement.** The slow search is only acceptable once the in-case bit has been seen; otherwise the firmware must stay as aggressive as it is today. | Changed: SET_DOWN never parks (QUIET only); CHARGING is no longer a state, charging is QUIET evidence; `IN_CASE` is the only path to PARK; link loss outside IN_CASE returns `RECONNECT_FAST`. Five tests cover the aggressive branches, and a second scenario trace (desk night) shows no PARK and a fast reconnect at 03:00. Consequence: the whole battery saving now depends on the phase 0 capture confirming `inCaseStatus` is real and readable while linked (items 2, 3). If it is not, the plan degrades to QUIET only. |

Open after the pass: items 2, 7, 12 and the C6 link-count discrepancy in 9.
They are measurement questions, not design questions, and phase 0/3 answer
them.

## 8. Downstream effects

| Area | Effect | Must keep doing |
|---|---|---|
| Event catalog / automations | New kinds `g2_charging`, `g2_discharging`, `g2_in_case`, `g2_out_of_case`, `g2_case_lid`, `g2_parked`, `g2_unparked`, ring equivalents. Catalog coverage test and the X-macro rows must be extended together. | Existing `g2_plugin_alive`/`g2_plugin_silent` semantics unchanged; automations keyed on them keep firing. |
| Notifications to glasses | A parked or in-case pair should not receive renders; the G2 sink drops with a counted `g2_drop` reason "parked/in case" rather than queueing. | Nothing lost silently: the NOTIF stats line gains the reason. |
| G2 one-temple repair | Must observe the policy park as a held intent; a park is not an episode. Outside IN_CASE the policy never touches repair or reconnect (RECONNECT_FAST is a no-op for them). | Genuine one-temple drops still repair; set-down and cable-charging glasses reconnect as fast as today. |
| Peer registry / boot | New intent kind; link-slot reservation for parked peers; boot reconnect unchanged (a parked peer at boot is simply auto-reconnect as today). | `bleautoreconnect <peer> off` still means off. |
| Phone pipe | Gains link slots while glasses are parked; must not be able to starve the glasses' return (item 9). | Console-on-pipe behaviour unchanged. |
| Status surfaces | `g2status`, `g2info`, web `g2-status` SSE, OLED/G2 Status pages show charge/case/parked. | Battery rows unchanged. |
| Settings registry | Five new persisted rows; appear on the Bluetooth pages (web, OLED, G2) through the existing registry-driven lists. | No settings-command auto-register; real per-setting commands per house rule. |
| Health logging | Ring parked while charging → no vitals; log rows show the gap, not stale values. | Logging session stays running. |
| Power policy | Unchanged. This plan does not touch clocks or sleep admission. The battery log is the instrument. | Adaptive default; busy flags. |
| Debug flags | A `debugg2presence` sub-flag under DEBUG_G2 for the state machine's transitions; the payload dump lives under existing `debugg2protocol`. | Flag bank drift is a build error (X-macro). |
| Docs | `G2_PROTOCOL.md` sid table (0x81 shape, field 13), `g2_proto/glasses_case.proto` comments, this file's Results section. | Never publish capture data; captures stay git-ignored. |
| Tests | Host: parser vectors (decoder oracle), policy machine (port of the Python tests), repair interaction, catalog coverage. Builds: p4x_eye full-debug, feathers3, xiao_s3 (bare `#if ENABLE_G2_GLASSES` gates are known latent breaks). | Green on every board, not just the P4. |

## 9. Sequence and effort

| Step | Depends on | Size | Hardware |
|---|---|---|---|
| 0a payload dump under DEBUG_G2 | — | tiny | reflash P4 |
| 0b labelled capture + decode + proto/doc update | 0a | one evening | glasses + case |
| 1 parsers, mirrors, events, status surfaces, host vectors | 0b | small | HW check of status pages |
| 2 policy: intent kind, QUIET flag in watchdog, park/reseek, settings, debug flag | 1 | medium | overnight runs |
| 3 three-night A/B/C measurement, write Results, choose default | 2 | calendar time | yes |

## 10. Decisions to settle

- Whether PARK applies to both temples only, or one temple can park while the
  other stays (no: the pair is the unit; a split pair is what repair exists to
  fix).
- Reseek ceiling default (60 min proposed) versus the worst-case morning
  reconnect delay the user accepts.
- ~~Whether charging-while-set-down parks after 5 min or waits for the full 20.~~
  Settled 2026-10-29: neither. Only in-case parks (after 5 min); set-down and
  cable charging never park and never get the slow reseek.
- What "in case" means for the ring: its battery frame reports charging, and
  the ring's charger is its case, so the ring's dock report is its in-case
  bit. Confirm when the ring policy is wired (not on the critical path).

## Results

(empty until phase 3)
