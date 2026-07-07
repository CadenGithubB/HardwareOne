# Implementation Prompt: ESP-NOW WPS-style "Pairing Mode" (OLED + core)

> **This document is a self-contained implementation brief for another coding
> agent (Fable).** It assumes no prior conversation. Everything needed —
> objective, design decisions, codebase map, step-by-step plan, an up/downstream
> impact analysis, security notes, and a test plan — is below. Line numbers are
> from a snapshot and **will drift**; anchor on the named symbols (functions,
> structs, enum members) and re-grep to confirm exact locations before editing.

Repo: `/Users/morgan/esp/hardwareone-idf` (ESP-IDF, target `esp32s3`, primary
board FeatherS3). Build: `source ~/esp/esp-idf/export.sh && idf.py build`.

---

## STATUS: IMPLEMENTED (builds clean) — validation deltas folded in

This plan was adversarially validated by 6 parallel code auditors, then
implemented. The final code differs from the original plan text below in these
ways — **read these before trusting the step-by-step sections, which reflect the
pre-validation design**:

1. **No payload fingerprint.** The RX path already drops frames whose header
   `meshFingerprint` (CRC16 of the mesh **label**, auto-stamped by `v4_send_frame`)
   matches no enabled local mesh (System_ESPNow.cpp ~4357), *before* dispatch. So
   "same mesh" is enforced for free upstream. `V4PayloadPairBeacon` carries only
   `{role, flags, reserved[2], deviceName[20]}` (24 B) — no fingerprint field.
   Hard precondition: pairing works only between devices on the **same mesh
   label + passphrase** (which is what `espnowsetpassphrase` produces — label
   defaults to `primary`). Factory-fresh (no mesh) devices can't pair (they have
   no passphrase → `espnowpairmode` refuses, and KEY_EX would fail).
2. **Handler lives in `System_ESPNow.cpp`** (not a header/second TU): there is a
   stale 6-field `V4RxCtx` duplicate in `System_ESPNow_Handlers_Crypto.cpp` that
   would misread `deviceName`/bools. `v4h_pair_beacon` uses the master 8-field ctx.
3. **Beacon flags = 0** (no `ACK_REQ` — would ACK-storm; no `BROADCAST_AUTH`).
   Sent via `v4_send_frame` to FF with a **fresh `generateMessageId()` each tick**
   (dedup drops repeats otherwise). Handler row placed in the unconditional table
   section, modeled on the HEARTBEAT row.
4. **No SD-capture edit / no type→string edit** — those surfaces don't exist for
   v4 opcodes (capture is dead code gated on `ver==3`; there is no opcode name
   table and no exhaustive `switch(type)`). Only two fan-out points touched: the
   enum and one handler-table row.
5. **Name sanitization is mandatory and implemented** (`pairModeSanitizeName`):
   `saveEspNowDevices` writes names raw into JSON, and beacon names are
   attacker-controlled. Whitelist `[A-Za-z0-9_-]`, space→`_`, else drop; empty →
   `peer_XXYYZZ`.
6. **Auto-pair only UNKNOWN MACs**, guarded by a 4-slot / 8 s per-MAC cooldown
   (`pairModeRecentlySeen`) before deferring — the cmd queue is only 6 deep, the
   registry caps at 16, and a spoofed beacon with a known MAC must not rename a
   registry entry. The heavy pair is deferred to `cmd_exec_task` via
   `submitDeferredToCmdExec` (never inline on `espnow_task`).
7. **OLED uses a self-re-arming ~1.2 s dirty window** (not a fixed 120 s one) so
   the countdown updates and stops within ~1.2 s of the window closing / leaving
   the view. Footer hint is `A:Toggle B:Back`. The manual-select UI was removed.
8. **`gMeshActivitySuspended`** pauses beacon TX during web activity; the window
   uses an **absolute** `millis()` deadline so it survives multi-second gaps.
9. **Beacon is BROADCAST_AUTH-signed** (post-review hardening). A MEDIUM found by
   an adversarial review: a plaintext beacon let an on-air attacker *without* the
   passphrase spray spoofed MACs to pollute the 16-slot registry + cause flash
   writes (registry-add happens before KEY_EX proves the passphrase). Fixed by
   sending the beacon with `ESPNOW_V4_FLAG_BROADCAST_AUTH` (mesh-group-key HMAC)
   and marking the handler row `V4_OPC_FLAG_REQ_AUTHENTICATED` — an outsider's
   beacon now fails the HMAC and is dropped after one cheap inline verify, before
   any registry mutation or deferred job. This also makes the *discovery* step
   itself require the shared passphrase, matching the original intent. (Residual,
   accepted: the beacon still leaks `deviceName` in cleartext — BROADCAST_AUTH is
   integrity, not confidentiality — and auto-pair is scoped to the **default**
   mesh's fingerprint/key.)

Symbols added: `ESPNOW_V4_TYPE_PAIR_BEACON=26`, `V4PayloadPairBeacon`,
`gPairModeUntilMs` + `espnowPairMode{Open,Close,Active,RemainingMs}`,
`cmd_espnow_pairmode` (registered), `sendPairBeacon`, `v4h_pair_beacon`,
`runDeferredPairModePair`, `pairMode{IsPaired,SanitizeName,RecentlySeen,MarkSeen}`.

Still HW-untested (operator tests on two devices — see §8 test plan).

---

## 1. Objective

Add a **WPS push-button style pairing mode** to the ESP-NOW subsystem: the user
toggles a timed "pairing window" on **both** devices; while both windows are
open the devices discover each other over the air and **securely pair**
automatically (no MAC typing, no list-picking). The window closes on timeout.
Expose this as a CLI command **and** as an OLED toggle screen.

This must establish **PAIRING**, never **BONDING** (see §2 — they are different
things in this codebase and must not be conflated).

---

## 2. Critical concept: PAIR ≠ BOND

- **Pairing** = a *peer relationship*. The peer lands in `gEspNow->devices[]`
  (+ the esp-now radio peer table + a `gMeshPeers`/`gMeshPeerMeta` cache slot),
  so the two devices appear in each other's Devices list and can exchange
  messages/files. `espnowpairsecure` additionally runs the **KEY_EX** handshake
  to stand up an encrypted unicast session. This is the layer we want.
- **Bonding** = the separate, heavier `@BOND` **authenticated command/RCE
  channel**, with its own per-session token derived from an X25519 shared
  secret (`bondconnect`, `ESPNOW_V4_TYPE_BOND_*`). **Out of scope. Do not touch
  the bond path.** The auto-pair must call the pair path only.

Verify while implementing: `cmd_espnow_pairsecure` establishes pairing + a
crypto session, and does **not** establish a bond. Confirm the deferred
auto-pair path never calls any `bond*` function.

---

## 3. Locked design decisions (do not re-litigate)

1. **Mutual (true WPS):** auto-pair fires **only when BOTH devices have the
   window open at the same time**. A same-mesh device sitting idle is never
   grabbed. (Mechanism: a device only *broadcasts* the pairing beacon while its
   own window is open; a receiver only *acts* on a beacon while its own window
   is open. Both conditions ⇒ both must have pressed "the button".)
2. **Secure pair only:** the pair is done via `espnowpairsecure` → KEY_EX. No
   "plain pair" path in this feature.
3. **Requires the shared mesh key:** refuse to open pairing mode unless a mesh
   passphrase is set (`gEspNow->encryptionEnabled`). KEY_EX is HMAC'd by the
   mesh bootstrap key, so pairing inherently requires the same passphrase — this
   is the intended security property, keep it.
4. **Timed window:** default **120 s**, clamp to ≤ 600 s. Runtime-only state
   (NOT persisted; a reboot leaves pairing mode off — correct).
5. **No backwards-compat concerns:** the operator owns all devices and erases
   flash before flashing, so wire-format changes are fine — no migration/shim
   code. Make clean breaking changes.

---

## 4. Why this is also a *discovery* fix (important context)

Two **fresh** same-mesh devices currently cannot find each other at all, for two
compounding reasons in `processMeshHeartbeats()` (System_ESPNow.cpp, ~7567):

- The heartbeat send is **gated**: `if (activePeerCount > 0 || pairedDeviceCount
  > 0)` (~line 7692). A device with no peers yet is silent.
- Even when it does send, `v4_broadcast_category(ESPNOW_V4_TYPE_HEARTBEAT, …)`
  **fans the heartbeat out to already-known `gMeshPeers` entries** (per-peer
  unicast), *not* an open RF broadcast. A never-heard device is never a
  recipient.

So there is no "hello, anyone out there?" signal today. The pairing beacon we
add (a **true FF:FF:FF:FF:FF:FF broadcast**, sent even with zero peers) is
exactly that missing signal. Do **not** try to solve this by relaxing the
heartbeat gate or repointing heartbeats at FF — keep the beacon a separate,
purpose-built broadcast so normal heartbeat behavior is untouched.

---

## 5. Codebase map (anchor on symbols, re-grep for lines)

**Wire / protocol** — `System_ESPNow_Wire.h`
- `enum EspNowV4Type : uint8_t` (~67). Discovery range is **20–29**; used: 20
  HEARTBEAT, 21 BOOT, 22 TOPO_REQ, 23 TOPO_START, 24 TOPO_PEER, 25 TIME_SYNC.
  **26–29 are FREE** → use `ESPNOW_V4_TYPE_PAIR_BEACON = 26`.
- `struct __attribute__((packed)) V4PayloadHeartbeat` (~187), 32 bytes, has a
  `static_assert(sizeof(...)==32)`. Model the new beacon struct + static_assert
  on this.

**Core** — `System_ESPNow.cpp`
- `void processMeshHeartbeats()` (~7567–8356): the ESP-NOW super-loop. Runs on
  **`espnow_task`** (core 0) every 10 ms via `espnowHeartbeatTaskFn` (~8358).
  Drains the RX ring (→ `onEspNowRawRecv` → handler dispatch, **inline on this
  task**), runs sweeps, sends the periodic heartbeat (~7663 `HB_INTERVAL_MS =
  5000`, gate ~7692), and does deferred bond housekeeping (~8140+). **Add the
  beacon TX block and the deferred-pair kick here.**
- `kV4HandlerTable[]` (~4176): entries are `{ type, flags, handler }`.
  `flags == 0` means *no requirement* (accepted plaintext, not REQ_PAIRED) — the
  KEY_EX and HEARTBEAT rows use `0`. `V4_OPC_FLAG_REQ_PAIRED` /
  `V4_OPC_FLAG_REQ_AUTHENTICATED` gate stricter types. **The beacon row must be
  `{ ESPNOW_V4_TYPE_PAIR_BEACON, 0, v4h_pair_beacon }`.**
- **Handler threading invariant** (read the comment block ~4156–4175): handlers
  run inline on `espnow_task` and must be bounded-time, allocation-light, **no
  FS I/O, no heavy crypto**. Heavier work MUST snapshot inputs into PSRAM and
  hand off via `submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg)`
  (~3242). Canonical pattern (see `runDeferredUserSync` ~3517, `runDeferredBond
  AuthFailLog` ~3280): `malloc` a ctx, copy inputs, `if
  (!submitDeferredToCmdExec(fn, ctx)) free(ctx);`; the fn does the heavy work on
  `cmd_exec_task` and `free`s ctx.
- `v4h_heartbeat(const V4RxCtx& ctx)` (~2796): the RX handler to **mirror** for
  `v4h_pair_beacon`. Sender MAC = `ctx.recv_info->src_addr`; payload =
  `ctx.payload` / `ctx.payloadLen`.
- `cmd_espnow_pairsecure(const String& argsInput)` (~12494): the full secure
  pair. Requires `gEspNow->encryptionEnabled`. Args `"<mac> <name> [mesh]"`.
  Handles the "peer already auto-appeared / other end paired first" race by
  **updating in place** (~12536–12566) — so simultaneous mutual pairing is
  already safe. Adds encrypted peer, seeds `gMeshPeers`, saves devices+peers,
  opens a per-peer rekey window via `espnowOpenPairingWindow(mac)` (see naming
  warning below), kicks `espnowKeyExInitiate(mac, meshLabel)`.
- `espnowKeyExInitiate(const uint8_t peerMac[6], const char* meshLabel)`
  (decl in `System_ESPNow_Handlers_Crypto.h` ~44): async KEY_EX initiate.
- Command table: the big `{ "espnowXXX", "desc", adminBool, cmd_fn, "usage" }`
  array (~14270–14335). Register `espnowpairmode` here.
- Helpers: `formatMacAddressBuf(mac, buf, size)` (~271), `formatMacAddress(mac)`
  (String), `isSelfMac(mac)` (inline, System_ESPNow.h ~1079),
  `meshFingerprintForLabel(label)` (~1141, static) and `gSettings.meshes[i]
  .fingerprint` (uint16). `MeshPeers::isHealthy(mac)` (System_MeshPeers.h).
- FF broadcast peer is registered at init (~8807–8820) — broadcasts work.
- **Plaintext broadcast send model:** `v4_send_topo_request(dst, reqId)` (~2212)
  already sends a v4 frame to `broadcastMac` (FF) in plaintext — model the
  beacon TX on it, or on the plaintext send primitive it calls. **Do NOT** use
  `v4_broadcast_category` (per-peer fan-out; won't reach undiscovered devices)
  and **do NOT** use the "smart"/encrypted unicast senders for the beacon.

**⚠ Naming collision — read carefully.** There is already an
`espnowOpenPairingWindow(mac)` — a **per-peer KEY_EX re-key window** (lets an
incoming KEY_EX replace a stored identity once). That is *unrelated* to this
feature's **global pairing-mode window**. Name the new global state
distinctly, e.g. `gPairModeUntilMs` / `espnowPairModeOpen/Close/Active/
RemainingMs`, and add a comment cross-referencing the difference so nobody
merges them.

**Message-type name / logging / capture:** grep for switches on
`h->type` / `ctx.type` and any type→string table; add `PAIR_BEACON` where an
exhaustive switch would otherwise warn or mislabel. Note the SD-capture heartbeat
skip filter at ~5199 (`espnowCaptureSkipHeartbeats`) — optionally also skip
PAIR_BEACON there so pairing bursts don't spam capture. (Also note: the comment
there says "types 7/14" but the enum is 20/90 — **trust the enum, not stale
comments** anywhere in this file.)

**OLED** — `OLED_ESPNow.cpp`
- The current working tree **already contains a first-pass manual pairing UI**
  (added earlier in the same effort): file-static `sPairScroll`,
  `sPairScrollInit`, `struct PairCandidate`, `sPairCandidates[]`,
  `sPairCandidateCount`, `sPairStatusMsg`, `sPairStatusUntil`, and functions
  `oledEspNowMacIsPaired`, `oledEspNowRefreshPairingList`,
  `oledEspNowPairSelected`, plus a rewritten `oledEspNowDisplayPairing`, a
  `case ESPNOW_VIEW_PAIRING` in `oledEspNowHandleInput`, a call in main-menu
  select `case 5`, a call in the 1 s periodic-refresh block inside
  `oledEspNowDisplay`, and a footer hint `"A:Pair B:Back"` in `OLED_Utils.cpp`
  (in the ESP-NOW `switch (gOledEspNowState.currentView)`). **Replace this
  manual-select UI with the toggle UI (§6.5). Remove now-dead helpers so nothing
  dangles or fails to compile.**
- OLED render facts: SSD1306 is 64px (header 10 / content y=11..53 / footer 10).
  Header+footer are drawn *after* content; `showHeader = !oledKeyboardIsActive()`.
  `executeOLEDCommand` / `executeOLEDCommandWithResult` are **synchronous**.

---

## 6. Implementation steps

### 6.1 Wire: beacon type + payload (`System_ESPNow_Wire.h`)
- Add `ESPNOW_V4_TYPE_PAIR_BEACON = 26` in the Discovery block.
- Add:
  ```c
  struct __attribute__((packed)) V4PayloadPairBeacon {
    uint16_t meshFingerprint;   // gSettings.meshes[0].fingerprint (early cross-mesh reject)
    uint8_t  role;              // gSettings.meshRole (informational)
    uint8_t  reserved;          // 0 (future flags)
    char     deviceName[20];    // for display + as the pair's local name
  };
  static_assert(sizeof(V4PayloadPairBeacon) == 24, "V4PayloadPairBeacon must be 24 bytes");
  ```

### 6.2 Core state + accessors (`System_ESPNow.cpp` + `System_ESPNow.h`)
- File-static: `static uint32_t gPairModeUntilMs = 0;` and a single pending-pair
  guard isn't needed (deferral handles concurrency) but a lightweight
  `static uint32_t gLastPairBeaconMs = 0;`.
- Public accessors (declare in `System_ESPNow.h`, guard for `ENABLE_ESPNOW`):
  ```c
  void     espnowPairModeOpen(uint32_t seconds);   // clamps ≤600
  void     espnowPairModeClose();
  bool     espnowPairModeActive();                 // gPairModeUntilMs && (uint32_t)millis() < gPairModeUntilMs
  uint32_t espnowPairModeRemainingMs();
  ```
  Use unsigned `millis()` diffs (wraparound-safe for a ≤10 min window).

### 6.3 Command `espnowpairmode` (`System_ESPNow.cpp` + table)
- `const char* cmd_espnow_pairmode(const String& args)`:
  - `RETURN_VALID_IF_VALIDATE_CSTR();` first (matches sibling commands).
  - If `!gEspNow || !gEspNow->initialized` → `"Error: ESP-NOW not initialized. Run 'openespnow' first."`.
  - Sub-arg `off`/`stop` → `espnowPairModeClose()`, return `"Pairing mode OFF."`.
  - Sub-arg `status` → report ON (+remaining s) / OFF.
  - Open (no sub-arg or a number of seconds): **require `gEspNow->encryptionEnabled`**, else
    `"Error: Set a mesh passphrase first (espnowsetpassphrase). Pairing needs the shared mesh key."`.
    Parse optional seconds (default 120, clamp ≤600), `espnowPairModeOpen(secs)`, return a
    message telling the user to open pairing on the other device too.
  - Register in the command table (`adminBool = true`, with a usage string).

### 6.4 Beacon TX + deferred auto-pair (`processMeshHeartbeats()`)
- **TX block** (near the heartbeat send, but NOT inside its peer gate): if
  `espnowPairModeActive()` and `now - gLastPairBeaconMs >= 1500`, set
  `gLastPairBeaconMs = now`, build a `V4PayloadPairBeacon` (fingerprint =
  `gSettings.meshes[0].fingerprint`, role, name = `gSettings.espnowDeviceName`),
  and send it **plaintext to FF** (model on `v4_send_topo_request`). This runs
  even with zero peers — that's the point.
- **RX handler** `v4h_pair_beacon(const V4RxCtx& ctx)` (register in
  `kV4HandlerTable` with flags `0`; add a min-length so short frames are
  dropped): inline, bounded. Steps:
  1. `if (ctx.payloadLen < sizeof(V4PayloadPairBeacon)) return;`
  2. `if (!espnowPairModeActive()) return;`  (mutual gate — we must be open too)
  3. `const uint8_t* mac = ctx.recv_info->src_addr;` `if (isSelfMac(mac)) return;`
  4. Reject cross-mesh early: `if (beacon->meshFingerprint != gSettings.meshes[0].fingerprint) return;`
  5. If already paired (`for i in devices[]: mac match`) → return.
  6. Optionally `noteMeshPeerRxActivity(mac, EspNowMeshRxKind::RxActivity)` so the
     peer also shows up in Devices/`espnowdevices` immediately.
  7. Snapshot `{mac, name}` into a `malloc`'d PSRAM ctx and
     `submitDeferredToCmdExec(runAutoPair, ctx)`; `if` it returns false, `free`.
     (Do **not** call `cmd_espnow_pairsecure` inline — violates the threading
     invariant: FS I/O + crypto.)
- **Deferred fn** `runAutoPair(void* arg)` (runs on `cmd_exec_task`): cast ctx;
  re-check `espnowPairModeActive()` and not-already-paired (state may have
  changed while queued); build `"<mac> <name>"` (replace spaces in name with
  `_`); call `cmd_espnow_pairsecure(args)`; log result via
  `BROADCAST_PRINTF` + `logSystemEvent("ESPNOW", "pair-mode auto-paired %s", mac)`;
  `free(ctx)`. Keep the window open for its remaining duration (allow pairing
  several devices). devices[] is capped at 16 — `cmd_espnow_pairsecure` returns
  a "maximum devices" error; surface it, don't crash.

### 6.5 OLED toggle UI (`OLED_ESPNow.cpp`, `OLED_Utils.cpp`)
- **Remove** the manual-select machinery listed in §5 (helpers + statics).
- `oledEspNowDisplayPairing`: 
  - Pairing OFF → "Pairing mode" / "OFF" / "Press A to start" / if
    `!gEspNow->encryptionEnabled` add "Set passphrase first".
  - Pairing ON → "Pairing ON  M:SS" (countdown from `espnowPairModeRemainingMs()`)
    / "Searching…" / a live list of paired peers (iterate `gEspNow->devices[]`,
    skip self) so devices appear as they pair in.
- `case ESPNOW_VIEW_PAIRING` in `oledEspNowHandleInput`: **A** toggles — if off,
  check encryption then `espnowPairModeOpen(120)` (or show the passphrase error);
  if on, `espnowPairModeClose()`. **B** → back to main menu. (Call the accessors
  directly; they're synchronous and cheap.)
- Main-menu select `case 5`: just set `currentView = ESPNOW_VIEW_PAIRING`
  (drop the `oledEspNowRefreshPairingList()` call).
- Periodic-refresh block in `oledEspNowDisplay`: remove the
  `ESPNOW_VIEW_PAIRING → oledEspNowRefreshPairingList()` branch (the toggle view
  reads live state each render; no rebuild needed).
- `OLED_Utils.cpp` footer hint for `ESPNOW_VIEW_PAIRING`: change `"A:Pair B:Back"`
  → `"A:Toggle B:Back"`.

---

## 7. Up/downstream impact analysis

**Upstream prerequisites (must hold for the feature to work):**
- **Same Wi-Fi channel.** ESP-NOW peers only hear each other on the same channel.
  If the two devices settle on different channels (e.g. one joined a different
  AP), beacons won't be received. This is *independent* of this change but is the
  #1 field gotcha — document it in the command/OLED help text and the test plan.
- Same mesh passphrase (KEY_EX HMAC). Enforced by the encryption gate.
- ESP-NOW initialized + FF broadcast peer registered (done at init).

**Downstream effects (this change touches / ripples into):**
- **New v4 opcode (26):** handler table (add row, flag 0), min-length gate, any
  exhaustive `switch(type)` and type→string tables, optional SD-capture skip.
  Wire struct + static_assert. Verify no code assumes the discovery range is
  contiguous/max-24.
- **RX acceptance:** with flag `0` the dispatcher accepts the plaintext broadcast
  and does not require paired/authenticated — verify against the
  `V4_OPC_FLAG_*` enforcement path so the beacon isn't silently dropped.
- **`espnow_task` load:** +1 broadcast every ~1.5 s **only while a window is
  open** (temporary). Beacon RX handler is inline but bounded. The heavy pair is
  correctly deferred to `cmd_exec_task` — RX drain is not stalled. Confirm the
  handler does zero FS/JSON/heavy-crypto inline.
- **`cmd_exec_task` load / persistence:** each auto-pair runs
  `cmd_espnow_pairsecure` = 2 FS writes (`saveEspNowDevices` + `saveMeshPeers`) +
  KEY_EX. Pairing N devices = N staggered jobs. Acceptable for a rare user
  action; note the flash-write cost. `devices[]` cap = 16.
- **Pairing-mode state is ephemeral** (not persisted) — correct; reboot ⇒ off.
- **Mutual-pair race** (both initiate KEY_EX simultaneously) is already handled
  by `cmd_espnow_pairsecure`'s update-in-place branch and KEY_EX in-flight
  dedup (`keyExIsInFlight`). No new race introduced. Verify.
- **Existing flows untouched:** explicit `espnowpairsecure`, the per-peer
  `espnowOpenPairingWindow` rekey window, normal heartbeats, topology, sensors.
  Regression risk to existing behavior is low because the feature is inert unless
  a window is opened.
- **Other UIs (web / BLE / G2):** unaffected; `espnowpairmode` is reachable via
  every CLI transport. A web ESP-NOW-page toggle is a nice follow-up but **out of
  scope**.
- **`gMeshPeers`/`gMeshPeerMeta`:** if you seed on beacon RX (step 6.4.6), a peer
  appears in `espnowdevices`/Devices list before pairing completes — desirable,
  but confirm it doesn't confuse the "unpaired" logic elsewhere.
- **Bond path:** explicitly NOT engaged. Auto-pair must never call `bond*`.
  Verify `cmd_espnow_pairsecure` establishes only pairing + session, no `@BOND`.
- **Audit/events:** emit `logSystemEvent("ESPNOW", …)` on auto-pair so the durable
  event log records it (consistent with existing ESPNOW events).

**Side effects / risks (and mitigations):**
- Beacon is plaintext ⇒ leaks device name + "in pairing mode" to a sniffer.
  Minor, physical, temporary. Acceptable.
- Any *other* same-mesh device with its window open will also pair (expected WPS
  behavior — you opened the window). Document.
- Cross-mesh beacon heard ⇒ fingerprint mismatch ⇒ skipped before any KEY_EX.
- devices[] full ⇒ graceful error surfaced by `cmd_espnow_pairsecure`.
- Beacon flood ⇒ auto-pair only when our window is open, per-MAC dedup via the
  already-paired check, deferred + naturally rate-limited by KEY_EX in-flight
  dedup. Bounded.
- Security-sensitive area: this adds an inbound path that can *initiate* a
  pairing/KEY_EX. Run the repo's `security-review` after implementing.

---

## 8. Test plan (HW — the operator tests on two real devices)

Build first: `idf.py build` (must be clean; watch the new `static_assert`s).

1. **Happy path:** both devices same passphrase, `espnowpairmode` (or OLED
   toggle) on both → within a few seconds they auto-pair. Verify on both:
   `espnowlist` shows the other with `encrypted=yes`; it appears in the OLED
   Devices list; a directed message/file works. `bondstatus` (or equivalent)
   shows **not bonded** (pairing ≠ bonding).
2. **Mutual gate:** open on only ONE device → **no** pairing occurs. Open the
   second → they pair.
3. **Passphrase gate:** no passphrase set → `espnowpairmode` refuses with the
   set-passphrase message; OLED shows the hint.
4. **Timeout:** open, don't pair, wait past the window → beacons stop, mode reads
   OFF; `espnowpairmode status` confirms.
5. **Channel mismatch (documented negative):** devices on different channels →
   no discovery (expected).
6. Serial logs to watch: beacon TX/RX, `[PAIRMODE]` auto-pair trigger, KEY_EX,
   pair result, and the `logSystemEvent` line.

---

## 9. Conventions & constraints (repo-specific — follow these)

- **No backwards-compat / migration code.** Clean breaking changes only.
- **DRAM is tight.** Prefer PSRAM (`EXT_RAM_BSS_ATTR`) for any non-trivial static
  buffers; do **not** spawn a task per action — reuse `processMeshHeartbeats` /
  `submitDeferredToCmdExec`.
- **Threading:** honor the RX-handler invariant (§5); heavy work off `espnow_task`.
- **Return contract:** command results should lead with `OK:`/`Error:` (a central
  `stampOkStatus` in `executeCommand` handles the `OK:` prefix — match sibling
  espnow commands' style; lead error strings with `Error:`).
- **Share logic, don't duplicate:** reuse `cmd_espnow_pairsecure` for the pair;
  don't reimplement peer-add/KEY_EX. If cleaner, extract its core into a helper
  `espnowSecurePairPeer(mac, name, meshId)` that both the command and
  `runAutoPair` call (optional refactor).
- **Do not commit** unless the operator says so; they test on hardware first,
  then commit. Commit style (if asked): `vX.Y.Z: plain-English what it does`.
- Edits go in the **main repo** (`/Users/morgan/esp/hardwareone-idf`), not any
  `.claude` worktree.

---

## 10. Deliverable checklist for Fable

- [ ] `ESPNOW_V4_TYPE_PAIR_BEACON = 26` + `V4PayloadPairBeacon` (+static_assert).
- [ ] `gPairModeUntilMs` + `espnowPairMode{Open,Close,Active,RemainingMs}` (decls in .h).
- [ ] `cmd_espnow_pairmode` + command-table registration (encryption-gated).
- [ ] Beacon TX in `processMeshHeartbeats` (FF broadcast, window-gated, ~1.5 s).
- [ ] `v4h_pair_beacon` handler + `kV4HandlerTable` row (flag 0, min-length) + defer via `submitDeferredToCmdExec`.
- [ ] `runAutoPair` deferred fn (re-check → `cmd_espnow_pairsecure` → log → free).
- [ ] OLED: replace manual-select pairing UI with the ON/OFF toggle + countdown + live paired list; update input, main-menu select, periodic-refresh, footer hint; remove dead helpers.
- [ ] Add/adjust type→string / capture-skip / exhaustive switches for the new opcode.
- [ ] `idf.py build` clean.
- [ ] Run `security-review` on the diff (new inbound pairing-initiation path).
- [ ] Confirm no `bond*` code is touched anywhere in the new paths.
