# R1 Ring Autoreconnect / Disconnect Handoff

**Date:** 2026-07-26  
**Audience:** Another AI (or human) continuing this work  
**Repos:** `hardwareone-idf` — BLE peer autoreconnect + R1 ring link drops  
**Status:** Autoreconnect recovers after unexpected drops; root cause of HCI `0x08` timeouts still open.

---

## 1. What this conversation covered

Three related topics, in order:

1. **How BLE autoreconnect works** — when connection is checked, when reseek is scheduled, boot vs mid-session.
2. **Symptom:** ring paired, `autoReconnect` ON, but not connected (until reboot / toggle).
3. **Live serial capture** of unexpected ring drops + successful reseeks, plus review of Commute773 RE repos for Ring insights.

No code changes were made in that conversation. This doc is the handoff.

---

## 2. Code map (where to look)

| Area | Files |
|------|--------|
| Peer registry + autoreconnect orchestrator | `components/hardwareone/BLE_Peers.cpp`, `BLE_Peers.h` |
| Main-loop tick / boot kick | `components/hardwareone/HardwareOne.cpp` (`bleAutoReconnectTick`, `bleBootReconnect`) |
| Ring connect / disconnect / TX | `components/hardwareone/G2_Ring.cpp` |
| Ring wire protocol | `components/hardwareone/System_R1_Protocol.{h,cpp}` |
| Protocol notes (heartbeat, opcodes) | `docs/R1_RING_PROTOCOL.md` |
| G2 peer “linked” = both temples | `G2_Glasses.cpp` (`g2BothConnected`, disconnect → `blePeerNoteLinkLost`) |

CLI:
- `bleautoreconnect r1-ring [on|off]`
- `blepeers`
- `ringconnect` / `ringdisconnect` / `ringstatus`

---

## 3. Autoreconnect behavior (reviewed)

### 3.1 Meaning of the flag

`autoReconnect` means: **reconnect to saved MAC after unexpected link loss** (and at boot). It does **not** mean “always keep trying forever” after every failure mode.

Intentional tear-down (`ringdisconnect`, `closeg2`, disconnect UI) stamps `sUserDisconnect` and **suppresses** reseek until cleared (successful link-up, boot reconnect heal, or toggling `bleautoreconnect … on` again).

### 3.2 When “am I connected?” is checked

**Checked on the main-loop tick**, not in the disconnect callback:

`HardwareOne.cpp` → every loop → `bleAutoReconnectTick()`.

For each peer with `sWantReconnect`:

1. Skip if user-disconnect stamped  
2. Skip unless `autoReconnect` (or one-shot `blePeerRequestReseek`)  
3. **`ops->isConnected()`** — if already up → `blePeerNoteLinkUp()` (clear backoff)  
4. If down and backoff due → `ops->connectSaved()`

Ring `isConnected` → `g2RingIsConnected()`.  
G2 `isConnected` → `g2BothConnected()` (both temples).

### 3.3 What schedules a reseek

| Trigger | Sets reseek? | Notes |
|---------|--------------|--------|
| BLE `onDisconnect` with prior link up | Yes → `blePeerNoteLinkLost` | Only if `autoReconnect` + saved MAC + not user-disconnect |
| Boot `bleBootReconnect` | One-shot `connectSaved` | Clears reconnect state via `blePeerNoteLinkUp` first |
| `bleautoreconnect … on` while down | Yes | Clears user stamp, then `blePeerNoteLinkLost` |
| Health Track mine while ring down | Yes (one-shot) | `blePeerRequestReseek` even if autoReconnect off |
| Failed connect (never was up) | **No** | Important gap — see §4 |

Backoff: **5s → 15s → 45s → 90s → 180s** (capped).

### 3.4 Ring saved-MAC connect path

`g2RingConnectSaved` → unified BLE worker job `RING_SAVED`:

- Waits up to 20s for both glasses (`g2WaitForBothConnected`), then 3s settle (or proceeds anyway).
- `ringPerformConnect(savedMac)` with **random-static** address type when MSB has `0xC0` bits set (required for R1; public-type connect times out ~30s).

---

## 4. Issue A — “autoReconnect ON but not connected”

### Observed

User had ring paired and UI/settings showed AutoReconnect ON, but ring stayed disconnected until reboot (after which boot reconnect worked).

### Likely causes (both real)

**A1 — Intentional disconnect stamp (by design)**  
After `ringdisconnect` / disconnect UI, `sUserDisconnect` blocks reseeks even while the persisted `autoReconnect` flag remains ON. Toggle `bleautoreconnect r1-ring on` again, or reboot (boot calls `blePeerNoteLinkUp` before kick).

**A2 — Boot / failed connect is one-shot (gap / bug)**  
`bleBootReconnect()` calls `connectSaved()` once and does **not** leave `sWantReconnect` set. Mid-session `bleAutoReconnectTick` only runs when `sWantReconnect` is true.  
`blePeerNoteLinkLost` only fires from disconnect when the link **was** up. A failed connect (ring asleep / out of range) never was up → **no retries** until something else schedules reseek.

### Quick unblock for operators

- Wake ring (tap), then `bleautoreconnect r1-ring on` or `ringconnect`
- Confirm `bleautoreconnect r1-ring` / `blepeers` shows a non-empty `mac1`

### Forward fix for A2 (recommended)

After boot kick (or after any failed `connectSaved` while `autoReconnect` is on), schedule the same backoff path as mid-session drops — e.g. call `blePeerNoteLinkLost` if still down after connect attempt completes, or set `sWantReconnect` + due time from boot so the tick keeps trying until linked or user disconnects.

---

## 5. Issue B — Unexpected mid-session drops (serial evidence)

### Capture summary (2026-07-26 ~18:25–18:31)

Boot after hijack reboot by user `red`:

| Time | Event |
|------|--------|
| 18:25:59 | `[RING] Connected to saved-ring` (boot reconnect OK) |
| 18:28:14 | Drop — HCI `rsn=0x8` |
| 18:28:34 | Reseek OK (~20s) |
| 18:28:53 | Drop again — `rsn=0x8` (~19s up) |
| 18:29:04 | Reseek OK |
| 18:30:59 | Drop again — `rsn=0x8` (~2 min up) |
| 18:31:20 | Reseek OK |

Pattern in logs:

```
W BT_APPL: gattc_conn_cb: if=… rsn=0x8
W BT_HCI: hcif disc complete: hdl 0x3, rsn 0x8 …
[RING] Dropped BLE link — ring is no longer connected
… later …
[RING] Connected to saved-ring (auth + time sync sent)
```

Also common after connect: `opcode:0x2013,status:0x12` (LE conn-param update rejected / unsupported) — side noise, not the disconnect reason.

Earlier `[G2] Firmware tore down widget (SYSTEM_EXIT) … Connection lost` is **lens UI / hijack**, not the ring BLE drop (ring died ~11s later with its own HCI timeout).

### Interpretation

- **`rsn=0x8` = Connection Timeout** — link-layer supervision expired; ring stopped answering.  
- **Not** intentional disconnect (no “User disconnect stamped”; reseeks happened).  
- **Autoreconnect is working** for these drops.  
- Connection lifetimes vary (~2 min / ~19s / ~2 min) — points at intermittent silence (sleep, RF contention with L+R temples, or app-layer kill), not a single fixed timer in our tick.

Useful log lines when capturing further:

- `[RING] Dropped BLE link` / `BLE onDisconnect`
- `[BLE-Peers] Link lost 'r1-ring'` / `Reseek 'r1-ring'` (may need `DEBUG_G2`)
- `[BLE-Peers] User disconnect stamped` (means no reseek by design)
- `[RING] BLE connect FAILED`

---

## 6. External RE repos (Commute773) — what we can learn

Repos reviewed:

- https://github.com/Commute773/g2-kit-unofficial — primary; `ble/ring.ts`, tests, docs  
- https://github.com/Commute773/g2-r1-re-tools-and-guide — methodology + early R1 case study  

### Useful for disconnects

1. **Duplicate “hash” / wire-prefix → disconnect ~15s**  
   In `g2-kit-unofficial/ble/ring.ts`: bytes `[1..4]` described as anti-replay hash; *“the ring will disconnect within ~15s if it sees the same hash twice.”*  
   In our stack those bytes are **Castagnoli CRC32 of the model** (same wire region). Exact **byte-identical retransmit** can look like a replay. The user’s **~19s** uptime drop is suspiciously close.

2. **Official app pair init is richer** than our `ringRunStandardSetup`  
   App (captures): pairAuth → config1/2 → time sync → **linkToGlasses ×2** → **HRV init**.  
   We do: pairAuth → systemTime → advStart.  
   `linkToGlasses` is mainly for tap/scroll relay via glasses; unclear if it affects link lifetime.

3. **Heartbeat**  
   Flutter host→ring heartbeat ~30s. We expose `R1Encoder::buildHeartbeat()` but comment says **not called yet**.  
   Our `docs/R1_RING_PROTOCOL.md` §6.19: host TX heartbeat did **not** clearly change connection lifetime in prior tests — weak candidate vs RF timeout.

### Less useful / outdated

- `g2-r1-re-tools-and-guide/guides/case-study-r1-ring.md` describes an early envelope (`00 NN 61 … 8a 03`) that does **not** match our Flutter-derived `System_R1_Protocol` frame. Prefer `ble/ring.ts` + our local protocol doc.
- Those repos do **not** document HCI supervision timeouts under a 3-link central (L temple + R temple + ring).

Our UUIDs / high-level cmd map already align with g2-kit; we are not starting from zero on the wire format.

---

## 7. Issue C — Duplicate CRC / anti-replay hypothesis

### Why it can happen in *our* code

Fresh `gR1Encoder.build*()` increments serial → new CRC almost always. Risk is **resending the same wire buffer**:

In `G2_Ring.cpp`, `ringDrainPendingLocked()`:

- On `ringWriteLocked` failure → `ringTxQPushFront(same bytes)`  
- If GATT reported failure but the ring already accepted the ATT write, the retry is an **exact CRC replay** → candidate for ~15s disconnect.

Health / sensorlog polls rebuild each time (lower risk). Coalesce replaces queue slots with newly built frames (also lower risk).

### Plan for (1) — duplicate-frame guard

**Phase 1 — Detect (do this first)**  
In `ringWriteLocked` (single choke point), before `writeValue`:

- Extract CRC from `data[1..4]`  
- Keep last N CRCs (e.g. 16) + timestamps for the current link session  
- If CRC seen within ~30s → log  
  `[RING] TX duplicate CRC=0x… len=… (possible anti-replay)`  
- Clear history on connect / `resetSerial()` / disconnect  

Correlate: if that log appears ~10–20s before `rsn=0x8`, hypothesis confirmed.

**Phase 2 — Harden retry (only if Phase 1 fires)**  

Preferred options (pick one, keep it simple):

1. On GATT failure after a write was attempted: **do not** `pushFront` identical bytes; drop or rebuild with new serial.  
2. Better long-term: queue stores **intent** (module/cmd/sub/payload), drain always rebuilds via encoder.  
3. Belt-and-suspenders: if CRC seen recently in `ringWriteLocked`, skip or rebuild-once before send.

**Do not** scatter checks across every poller — fix the TX path.

---

## 8. Recommended forward plan

### P0 — Keep gathering evidence

- Leave serial open with G2/RING debug useful.  
- On next drop cluster, capture **5–10s before** `rsn=0x8` (any `[RING] TX`, health polls, image push, conn-param noise).  
- Note whether ring is worn vs idle, and whether Health page / heavy G2 TX is active.

### P1 — Duplicate CRC detector

- Implement Phase 1 logger in `ringWriteLocked` (§7).  
- No behavior change until confirmed.

### P2 — Boot / failed-connect reseek loop

- Close gap A2: if `autoReconnect` and saved MAC and still down after boot/`connectSaved` failure, arm `sWantReconnect` + backoff so tick keeps trying (respect user-disconnect).  
- This fixes “flag ON, forever idle after one failed boot attempt.”

### P3 — If duplicate CRC confirmed

- Change TX queue retry semantics (§7 Phase 2).  
- Re-test overnight for fewer ~15–20s kills.

### P4 — If duplicates never appear

Treat remaining `0x08` as RF / sleep / multi-link contention:

- Consider host heartbeat again despite prior weak results (cheap experiment).  
- Review conn-param update failures (`0x2013` / `0x12`) and glasses priority while ring is up (code already drops temples to BALANCED during ring connect via `GlassesPriorityGuard`).  
- Optionally compare setup sequence to g2-kit `linkToGlasses` / config / HRV init (lower priority unless tap-relay bugs show up).

### P5 — Out of scope unless requested

- Temple `ringbridge` / sid=0x90 relay — already documented as dead-end in `R1_RING_PROTOCOL.md` §13; commands unregistered.

---

## 9. Working conclusions (as of 2026-07-26)

1. Mid-session autoreconnect **works** for unexpected drops (log proves reseek + reconnect).  
2. “Flag ON but disconnected” is explained by **user-disconnect suppress** and/or **one-shot boot reconnect with no failure retry**.  
3. Drops themselves are HCI **Connection Timeout (`0x08`)**, not CLI disconnect.  
4. Best protocol-level lead from g2-kit: **identical CRC retransmit → ~15s disconnect**; our TX fail→retry path is the place to instrument.  
5. Longer ~1–2 min timeouts may still be ring sleep or radio contention with dual temples — not yet proven.

---

## 10. Suggested first commits / PR shape (when implementing)

Keep changes small and separable:

1. **PR: ring TX duplicate-CRC log** (detect only).  
2. **PR: bleBootReconnect / failed connect arms mid-session backoff** (A2 fix).  
3. **PR (conditional): TX queue never retransmits identical wire bytes** — only after detector evidence.

Avoid bundling heartbeat / linkToGlasses / conn-param experiments into the same PR until P0/P1 clarify the drop mode.
