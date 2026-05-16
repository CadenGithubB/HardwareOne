# ESPNOW V4 — Phase 1 Implementation Plan

**Parent plan:** [docs/ESPNOW_V4_PLAN.md](ESPNOW_V4_PLAN.md)
**Phase 0 status:** ✅ landed (handler-table dispatch + Wire.h extraction)
**Phase 1 goal:** V4 wire-format cutover, identity propagation fix, drop CMD_RESP truncation
**Estimate:** 3–4 focused days
**Risk level:** Medium — touches every send/recv site in the codebase, but Phase 0 isolated per-opcode logic so handler bodies stay untouched

---

## TL;DR

Phase 1 establishes V4 as the on-wire protocol — a 32-byte header with reserved fields for sessions and mesh fingerprints that Phases 2–3 will fill in, plus the V4 opcode numbering scheme. The work is mostly mechanical (rename `v3_*` → `v4_*` symbols, update header writes, renumber opcodes). The non-mechanical piece is **lifting the 2 KB `CMD_RESP` cap by streaming CLI output through `STREAM_FRAME` opcodes during execution** — that's the only place this phase introduces real behavior change.

After Phase 1:
- All on-wire frames are V4 (magic `0x3148`, version byte `4`, 32-byte header).
- Inbound ESPNOW commands carry `ctx.auth.ip = "espnow:<mac>"`, making the existing `startstream` gate work and giving logs unambiguous origins.
- `meshrunfile` / `meshstatus` / any large CLI output relayed via `espnowremote` returns in full instead of truncating at 2 KB.
- Header has reserved bytes (sessionId, frameSeq, meshFingerprint) that Phases 2–3 populate without another wire break.

---

## Deliverables, in dependency order

### 1. Wire schema bump (V3 → V4) — half day

**Files:** `components/hardwareone/System_ESPNow_Wire.h`

The new header layout:

```
Offset Size Field            Phase 1 use
-----------------------------------------------------------
0      2    magic            0x3148 (unchanged from V3)
2      1    version          4
3      1    type             V4 opcode (see table below)
4      2    flags            16-bit (V3 was 8-bit) — see flag table
6      1    headerLen        32
7      1    reserved1        0 (Phase 1+)
8      4    msgId            unchanged
12     6    originMac        unchanged
18     1    ttl              unchanged
19     1    fragIndex        unchanged
20     1    fragCount        unchanged
21     1    reserved2        0 (Phase 1+)
22     2    meshFingerprint  0 in Phase 1; Phase 2 populates from gSettings
24     2    sessionId        0 in Phase 1; Phase 3 populates
26     4    frameSeq         0 in Phase 1; Phase 3 populates
30     2    crc16            unchanged location/semantic
```

Total: **32 bytes**. Payload max: `250 − 32 = 218` bytes (was 226 in V3 — 8-byte shrink).

**Why these sizes:**
- 2-byte meshFingerprint = 64K distinct mesh labels with birthday-collision at ~256. For N_MESHES=4 fleets, more than enough. Saves 2 bytes vs the 4-byte fingerprint sketched in V4_PLAN.md.
- 2-byte sessionId = 65K concurrent sessions per peer pair. Way more than needed (sessions rotate on reboot or every 24h).
- 4-byte frameSeq = 4 billion frames per session. At 100 fps that's 14 months of one direction without rotation. Plenty.
- 16-bit flags lets us define the 11 currently-known flag bits with 5 spare for future extensions without another wire break.

**Flag bit definitions** (16-bit):

```cpp
enum EspNowV4Flags : uint16_t {
  ESPNOW_V4_FLAG_ACK_REQ        = 0x0001,
  ESPNOW_V4_FLAG_BROADCAST_AUTH = 0x0002,  // Phase 3+: HMAC'd with mesh group key
  ESPNOW_V4_FLAG_SESSION_FRAME  = 0x0004,  // Phase 3+: AEAD-wrapped with session key
  ESPNOW_V4_FLAG_HANDSHAKE      = 0x0008,  // Phase 3+: key-exchange / session message
  ESPNOW_V4_FLAG_STREAM_BEGIN   = 0x0010,
  ESPNOW_V4_FLAG_STREAM_END     = 0x0020,
  ESPNOW_V4_FLAG_PRIORITY_HIGH  = 0x0040,  // Phase 5+: bump above retry queue
  ESPNOW_V4_FLAG_COMPRESS       = 0x0080,  // reserved
  // 0x0100–0x8000 reserved
};
```

In Phase 1, only the bits that V3 used are active: `ACK_REQ`, `STREAM_BEGIN`, `STREAM_END`. The crypto/session flags are defined but never set. Phase 3 will start enforcing "exactly one of BROADCAST_AUTH / SESSION_FRAME / HANDSHAKE on unicast"; Phase 1 leaves that rule loose.

**Note on `ESPNOW_V3_FLAG_ENCRYPTED`:** This flag goes away in V4. V3 used it as a per-frame opt-in for radio-layer LMK encryption. In V4 (Phase 1), all unicast frames still ride radio-layer LMK encryption the same way V3 did — the difference is the flag bit is implicit, not announced. Phase 3 replaces radio-layer LMK with session-key AEAD and uses `SESSION_FRAME` to mark that.

### V4 opcode table

The current V3 numbering (1–32) gets replaced by category-based ranges with reserved slots:

| Range | Category | V4 opcodes (Phase 1 lands these) | Reserved slots (Phase 3/4/5 add) |
|---|---|---|---|
| 1–9 | Transport | 1=ACK | 2=NACK, 3=FRAG_REQ, 4=FRAG_REPLY |
| 10–19 | Crypto | (none in Phase 1) | 10=KEY_EX_HELLO, 11=KEY_EX_REPLY, 12=KEY_EX_CONFIRM, 13=SESSION_OPEN, 14=SESSION_CONFIRM, 15=SESSION_CLOSE, 16=SESSION_REKEY |
| 20–29 | Discovery | 20=HEARTBEAT, 22=TOPO_REQ, 23=TOPO_START, 24=TOPO_PEER, 25=TIME_SYNC | 21=HEARTBEAT_BROADCAST, 26–29 reserved |
| 30–49 | App unicast | 30=CMD, 31=CMD_RESP_STATUS, 32=TEXT, 33=METADATA_REQ, 34=METADATA_RESP, 35=METADATA_PUSH, 36=USER_SYNC | 37–49 reserved |
| 50–59 | Streaming | 50=STREAM_FRAME (was STREAM), 51=STREAM_CTRL | 52–59 reserved |
| 60–69 | Files | 60=FILE_START, 61=FILE_CHUNK (was FILE_DATA), 62=FILE_END | 63=FILE_ACK, 64=FILE_PROGRESS, 65=FILE_CANCEL (Phase 4 wires them up) |
| 70–79 | Events | (none in Phase 1) | 70=SUBSCRIBE, 71=UNSUBSCRIBE, 72=EVENT, 73=SUB_LIST_REQ, 74=SUB_LIST_REPLY (Phase 5) |
| 80–89 | Sensors | 80=SENSOR_BROADCAST, 81=SENSOR_DATA, 82=SENSOR_STATUS, 83=WORKER_STATUS | 84–89 reserved |
| 90–99 | Bond | 90=BOND_HEARTBEAT, 91=BOND_CAP_REQ, 92=BOND_CAP_RESP, 93=MANIFEST_REQ, 94=SETTINGS_REQ, 95=BOND_STATUS_REQ, 96=BOND_STATUS_RESP | 97–99 reserved |
| 200–255 | User-defined | (none) | — |

**Mapping table (V3 → V4 for migration):**

| V3 name (number) | V4 name (number) |
|---|---|
| ACK (1) | ACK (1) |
| BOND_CAP_REQ (2) | BOND_CAP_REQ (91) |
| BOND_CAP_RESP (3) | BOND_CAP_RESP (92) |
| TEXT (4) | TEXT (32) |
| CMD (5) | CMD (30) |
| CMD_RESP (6) | CMD_RESP_STATUS (31) — semantics change, see §4 |
| HEARTBEAT (7) | HEARTBEAT (20) |
| FILE_START (8) | FILE_START (60) |
| FILE_DATA (9) | FILE_CHUNK (61) |
| FILE_END (10) | FILE_END (62) |
| MANIFEST_REQ (11) | MANIFEST_REQ (93) |
| MANIFEST_RESP (12) | — (dead opcode in V3, removed) |
| STREAM (13) | STREAM_FRAME (50) |
| BOND_HEARTBEAT (14) | BOND_HEARTBEAT (90) |
| SENSOR_DATA (15) | SENSOR_DATA (81) |
| SETTINGS_REQ (16) | SETTINGS_REQ (94) |
| SETTINGS_RESP (17) | — (dead opcode in V3, removed) |
| SETTINGS_PUSH (18) | — (already reserved) |
| METADATA_REQ (19) | METADATA_REQ (33) |
| METADATA_RESP (20) | METADATA_RESP (34) |
| METADATA_PUSH (21) | METADATA_PUSH (35) |
| TIME_SYNC (22) | TIME_SYNC (25) |
| TOPO_REQ (23) | TOPO_REQ (22) |
| TOPO_START (24) | TOPO_START (23) |
| TOPO_PEER (25) | TOPO_PEER (24) |
| USER_SYNC (26) | USER_SYNC (36) |
| WORKER_STATUS (27) | WORKER_STATUS (83) |
| SENSOR_STATUS (28) | SENSOR_STATUS (82) |
| SENSOR_BROADCAST (29) | SENSOR_BROADCAST (80) |
| BOND_STATUS_REQ (30) | BOND_STATUS_REQ (95) |
| BOND_STATUS_RESP (31) | BOND_STATUS_RESP (96) |
| STREAM_CTRL (32) | STREAM_CTRL (51) |

The dead opcodes (`MANIFEST_RESP`, `SETTINGS_RESP`) get removed entirely — they were never sent or received in V3 (manifests/settings travel as files via `FILE_END` for specially-named payloads).

### 2. Symbol rename: `v3_*` → `v4_*` — half day

**Files:** `components/hardwareone/System_ESPNow.cpp`, callers in `System_ESPNow_Sensors.cpp`, `WebPage_ESPNow.cpp`, `WebPage_ESPNow_Metadata.cpp`, `OLED_ESPNow.cpp`, `G2_Page_ESPNow.cpp`

Functions and types to rename:
- `EspNowV3Header` → `EspNowV4Header`
- `EspNowV3Type` → `EspNowV4Type`
- `EspNowV3Flags` → `EspNowV4Flags`
- `ESPNOW_V3_TYPE_*` → `ESPNOW_V4_TYPE_*` (with new numeric values per the table above)
- `ESPNOW_V3_FLAG_*` → `ESPNOW_V4_FLAG_*`
- `ESPNOW_V3_MAGIC` (stays the same value `0x3148`; rename the constant for consistency)
- `ESPNOW_V3_MAX_PAYLOAD` → `ESPNOW_V4_MAX_PAYLOAD` (value changes: 218 not 226)
- `v3_send_frame`, `v3_send_chunked`, `v3_send_ack`, `v3_send_frag_ack`, `v3_send_topo_start`, `v3_send_topo_peer`, `v3_send_user_sync`, `v3_send_command_response`, `v3_broadcast`, `v3_broadcast_text` → all `v4_*`
- `v3_dedup_*`, `v3_reasm_*` → `v4_*`
- `v3_dispatch_table_try`, `v3_dispatch_lookup`, `v3_dispatch_post_cleanup`, `v3_try_handle_incoming` → `v4_*`
- All `v3h_*` handler function names → `v4h_*`
- All `V3OpcodeEntry`, `V3OpcodeHandler`, `V3RxCtx`, `kV3HandlerTable[]`, `V3_OPC_FLAG_*` → `V4*`
- All comment text strings `[V3_RX]`, `[V3_FRAG_RX]`, `[V3_DEDUP]`, `[V3_FILE_TX]`, `[V3_FILE_RX]`, etc. → `[V4_*]` so log output is unambiguous

**Rename strategy:** sed-style mechanical rename per identifier, compile, fix anything that breaks. Phase 0's handler-table refactor isolated the per-opcode logic, so the renames are tedious but boring — no algorithmic change.

### 3. Header field updates — half day

**Files:** `components/hardwareone/System_ESPNow.cpp`

Every site that writes a header needs to:
- Set `version = 4`
- Set `headerLen = 32`
- Set `flags` field to 16-bit (was 8-bit) — careful at the byte-pack site
- Zero out the new reserved fields (`reserved1`, `reserved2`, `meshFingerprint`, `sessionId`, `frameSeq`)
- The CRC computation moves from end-of-header (offset 22 in V3) to offset 30 in V4 — the CRC is still over the *payload*, not the header, so the math doesn't change, just the field location

Every site that reads a header needs to:
- Validate `version == 4` (was `== 3`)
- Validate `headerLen == sizeof(EspNowV4Header)` (32, was 24)
- Fragmentation reassembly code uses `V4_MAX_FRAGMENT_PAYLOAD` — recompute as `218 − 16 = 202` if we keep the 16-byte reserved-for-fragment-overhead pattern, or just `218 − 0 = 218` if we want full payload on each fragment. (V3 was `200 = 226 − 26`; the math was generous. V4 can keep the same conservative approach: `V4_MAX_FRAGMENT_PAYLOAD = 192`.)

**Fragmentation impact:** Max payload per fragment shrinks from 200 to 192 bytes (8-byte loss from larger header). Max reassembled message stays at `16 × 192 = 3072` bytes (was 3200). Acceptable; no caller relies on the 3200 ceiling.

### 4. CMD_RESP capacity bump — half day (revised from "streaming refactor")

**Update during execution:** the original sub-task was misframed as "drop the
2 KB CMD_RESP cap by switching to streaming." Reading the code revealed the
streaming was already in place — `broadcastOutput` calls during a remote-CMD
execution are already routed to STREAM frames (one frame per log line),
gated by `gCurrentStreamCmdId`. The 2 KB cap was on the FINAL return value
only, which is typically short (`"OK"`, short status strings).

The actual fix was a capacity bump:
- `V4_FRAG_MAX` 16 → 32 (max fragmented message 3.2 KB → 6.4 KB)
- CMD_RESP cap 2 KB → 6 KB (sender truncation + receive buffer)
- `deferredCmdRespResult` alloc 2048 B → 6144 B

PSRAM cost: +6.4 KB for the reassembly buffer, +4 KB for the cmdResp buffer.
DRAM impact: zero. Covered in detail in conversation; logged here for the
audit trail.

Original sub-task §4 below described what the streaming refactor WOULD have
been if it'd been needed — kept for reference in case streaming becomes
necessary for larger payloads in the future.

### 4 (deferred / not needed in Phase 1). CMD_RESP true streaming refactor

**Files:** `components/hardwareone/System_ESPNow.cpp`, possibly `System_Utils.cpp`

**Current V3 behavior:**
- Inbound CMD arrives → `v3h_cmd` defers to task → task runs `executeCommand` → output is buffered in `gEspNow->deferredCmdRespResult` (2 KB capped) → after command completes, single `CMD_RESP` frame sent back with `(success_flag, result_text)`.
- Anything past 2 KB silently truncates.
- Large commands (`meshstatus` with many peers, `help all`, `userlist json`, etc.) overflow.

**New V4 behavior:**
- Inbound CMD arrives → handler defers to task → task creates a stream session targeting the requester → all CLI output during execution routes through the stream session → final `CMD_RESP_STATUS` frame carries only `(exit_status, optional_short_message)`.
- The streaming primitives already exist: `createStreamSession()`, `sendSessionStreamFrame()`, `destroyStreamSession()`, with the `STREAM_BEGIN` / `STREAM_END` flag pattern.
- Response opcode `CMD_RESP` (V3 #6, 2KB payload) becomes `CMD_RESP_STATUS` (V4 #31, ~16-byte payload):
  ```c
  struct __attribute__((packed)) V4PayloadCmdRespStatus {
    uint8_t  success;      // 1=OK, 0=fail
    uint8_t  exitCode;     // optional, for future
    uint16_t reserved;
    char     finalMsg[12]; // short status string, e.g., "OK" / "FAIL: timeout"
  };
  ```

**Implementation steps:**

a. In `v4h_cmd` (the migrated handler), open a STREAM session targeting `ctx.recv_info->src_addr` with `cmdMsgId = h->msgId`. Store the session handle alongside the deferred CMD state.

b. In the cmd_exec task path that processes the deferred command, install the per-task output capture so it routes through the stream session instead of (or in addition to) the buffered output.

c. Audit every site in `System_ESPNow.cpp` and `System_Utils.cpp` that emits command output during command execution. Verify each one routes through the stream session:
   - `broadcastOutput()` calls
   - `BROADCAST_PRINTF` macro invocations
   - Direct `Serial.println` calls during command execution (should be rerouted via the output mask)
   - `httpd_resp_send` for web — not relevant for ESPNOW-originated commands
   - `ctx.replyHandle` for the queued-response pattern

d. After `executeCommand` returns, send `CMD_RESP_STATUS` with `success` from the return value, optional `finalMsg` truncated to 12 bytes. Close the stream session.

e. On the **sender** side (the device that issued `espnowremote`), the existing STREAM frame receive path already prints stream output to the local console. Plumb the receiver to also collect the stream output into a buffer for return to the caller (web, CLI, etc.). The `CMD_RESP_STATUS` arrival signals "stream complete, exit status is X."

**Risk:** an output path that doesn't route through the stream session will drop output. Mitigation: read-through audit + on-device test with a known-large output command (`espnowremote <peer> meshstatus` after creating a fake mesh with 50 fake peers, or `espnowremote <peer> "help all"`).

### 5. `ctx.ip` propagation fix — 30 minutes

**File:** `components/hardwareone/System_ESPNow.cpp` (the migrated `v4h_cmd` function and `v3_handle_cmd` body)

Inside `v3_handle_cmd` (the task-context function that builds the `Command` struct and submits it to cmd_exec), after the existing auth lookup, add:

```cpp
char macStr[18];
formatMacAddressBuf(srcMac, macStr, sizeof(macStr));
cmd.ctx.auth.ip = String("espnow:") + macStr;
```

Verify:
- `cmd_espnow_startstream` at `System_ESPNow.cpp:9483` now has its `if (!currentAuthContext().ip.startsWith("espnow:"))` gate working correctly — only ESPNOW-originated calls pass.
- Audit log lines for inbound ESPNOW commands show `[CMD] user@espnow:AA:BB:CC:DD:EE:FF: <command>` instead of just `[CMD] user@espnow: ...`.

### 6. Cleanup — half day

After the renames land:
- Remove dead opcodes from V4 wire enum (`MANIFEST_RESP`, `SETTINGS_RESP` if not used).
- Remove the V3 magic / version constants entirely (no V3 frames are sent or accepted).
- Update `docs/ESPNOW_V4_PLAN.md`'s wire-format section if anything changed during implementation (likely the meshFingerprint size: I'm proposing 2-byte here vs 3-byte in the parent plan).
- Update `docs/ESPNOW_TRANSPORT_PLAN.md` with a "superseded" note if not already there.

---

## File-level edit catalog

### Modified

- `components/hardwareone/System_ESPNow_Wire.h` — complete rewrite of header/flag/opcode definitions
- `components/hardwareone/System_ESPNow.cpp` — symbol renames, header writes, CMD_RESP streaming, ctx.ip
- `components/hardwareone/System_ESPNow.h` — any V3-named struct fields referenced (e.g. `EspNowState::deferredCmdRespResult` field stays but its size cap goes away)
- `components/hardwareone/System_ESPNow_Sensors.cpp` — V3 → V4 symbol renames (uses TX/RX paths)
- `components/hardwareone/WebPage_ESPNow.cpp` — V3 → V4 in any direct opcode references (unlikely; mostly calls high-level send functions)
- `components/hardwareone/WebPage_ESPNow_Metadata.cpp` — same
- `components/hardwareone/OLED_ESPNow.cpp` — same
- `components/hardwareone/G2_Page_ESPNow.cpp` — same
- `components/hardwareone/System_Utils.cpp` — verify output routing covers all command-execution paths

### Unchanged

- `WebPage_Bond.cpp`, `WebPage_Bond.h` — call CLI commands by name (e.g. `bondconnect`); user-facing surface untouched
- The dispatch table itself (Phase 0's structure stays as-is; just opcode constants change)
- All V3/V4 per-opcode handler bodies — Phase 0 isolated them; Phase 1 doesn't change behavior inside `v4h_cmd`, `v4h_text`, etc.

---

## Verification checklist (in execution order)

1. **Compile clean after wire-schema bump.** Catches misnamed identifiers and missed renames immediately.
2. **`v4_try_handle_incoming`'s validation rejects V3 frames.** Synthetic test: forge a V3-version-byte frame, verify log line `[V4_RX] REJECTED: ver=3 (expected 4)` and drop.
3. **Two-device wipe-and-reflash.** Both devices on V4. Pair, send TEXT, run remote CMD, observe full bond sync (CAP + MANIFEST + SETTINGS via files). All should work because the per-opcode handler bodies are unchanged.
4. **`ctx.ip` lands correctly.** Run `espnowremote <peer> wifistatus` from the master; on the peer's serial log, the audit line should be `[CMD] asd@espnow:E8:9F:6D:31:51:60: wifistatus` (not the old empty-ip variant).
5. **`startstream` gate works.** Try calling `cmd_espnow_startstream` from a serial CLI session directly (`SOURCE_SERIAL`, no `espnow:` prefix) — should reject with the "wrong-transport" error. Then trigger it via an inbound ESPNOW CMD — should succeed.
6. **Large CMD_RESP streams.** Pair two devices, run `espnowremote <peer> "help all"` (or any command with >2KB output). Verify the master receives the full output, not truncated at 2 KB.
7. **Fragmentation still works.** Send a >218-byte payload (e.g. a long TEXT message). Verify fragmentation reassembles correctly. The 8-byte header growth means existing fragmentation tests that hovered near boundaries may need re-checking.
8. **All V4 opcodes round-trip.** Exercise each migrated opcode once: ACK, HEARTBEAT, TEXT, CMD, FILE_*, METADATA_*, USER_SYNC, TIME_SYNC, TOPO_*, all BOND_*, SENSOR_*, STREAM_*. Bond mode end-to-end covers most of these in one test.

---

## Footguns

- **Field-rename grep traps.** `v3` appears in unrelated identifiers (`v3.something`, `0x3v` hex constants) — restrict renames to whole-word matches with `\b` boundaries. Always compile after each batch.
- **Endianness on the wire.** The new `meshFingerprint` (2-byte), `sessionId` (2-byte), `frameSeq` (4-byte), `flags` (now 16-bit) fields are multi-byte. The V3 protocol was little-endian via direct struct packing (ESP32 is little-endian, peer is also ESP32, no conversion). Keep that — don't introduce host-to-network byte swaps.
- **Header alignment in `__attribute__((packed))`.** With reserved bytes at offset 7 and 21, the struct should pack cleanly to 32 bytes. Verify with `static_assert(sizeof(EspNowV4Header) == 32)`.
- **CRC offset.** V3's CRC was at offset 22. V4's is at offset 30. Anything that computes CRC by raw offset (rather than via `&h->crc16`) needs updating. Best practice: always reference via the struct field, never a raw offset.
- **Output-routing tail risk.** Most CLI commands route output through `broadcastOutput()` and the per-task capture state. Any command that does direct `Serial.println` or some bespoke output method during execution will silently drop output in the streaming path. The audit pass in step 4(c) is the only defense; allocate time for it.
- **`espnowremote` callers.** The CLI command builds a CMD frame and waits for `CMD_RESP`. With V4's new "exit status only" CMD_RESP_STATUS, the caller needs to collect stream frames AND wait for the status frame. Don't break the existing CLI command flow.
- **`espnowusersync` and credential frames.** Phase 1 still uses V3-style passphrase-LMK encryption at the radio layer for unicast. USER_SYNC's `ESPNOW_V4_FLAG_ENCRYPTED` check (formerly `V3_FLAG_ENCRYPTED`) needs a Phase 1 equivalent — easiest is to keep the V3 ENCRYPTED flag bit (currently `0x02`, conflicts with `BROADCAST_AUTH` in the new flag table). **Resolution:** rename `ESPNOW_V3_FLAG_ENCRYPTED` to a Phase 1 transitional flag, e.g., `ESPNOW_V4_FLAG_LMK_ENCRYPTED` at bit `0x0100` (in the reserved range). Phase 3 will deprecate this flag entirely in favor of `SESSION_FRAME`.

---

## Decisions to lock before starting

1. **meshFingerprint size — 2 or 4 bytes?** Parent plan said 4 bytes (3-byte packed). This doc proposes 2 bytes (16-bit hash, birthday collision at ~256 labels). **Recommendation: 2 bytes** (saves 2 bytes of header for negligible collision risk in a 4-mesh fleet). Push back if you want the larger fingerprint.
2. **Keep or drop the V3 `ENCRYPTED` flag in V4?** Phase 1 still needs to mark which frames ride LMK encryption (USER_SYNC requires it). **Recommendation:** keep as `ESPNOW_V4_FLAG_LMK_ENCRYPTED` at bit `0x0100`, deprecate in Phase 3. Trivial; doesn't waste a slot since Phase 3 will reclaim it.
3. **Should `CMD_RESP_STATUS` carry stream session ID for correlation?** With multiple inflight commands, the sender needs to know which stream goes with which CMD_RESP_STATUS. **Recommendation:** the `msgId` in the V4 header already serves this — the sender used `msgId` for the CMD, all stream frames echo it, and CMD_RESP_STATUS echoes it. No extra field needed.
4. **Wire the new opcodes (KEY_EX_*, SUBSCRIBE, etc.) in Wire.h now or wait until their phase?** **Recommendation:** define them in the V4 enum now (as reserved slots) so Phase 3/4/5 don't risk renumbering. Defining costs nothing; the handlers don't need to exist until the relevant phase.
5. **Drop dead V3 opcodes (`MANIFEST_RESP`, `SETTINGS_RESP`)?** **Recommendation:** drop them. They were never sent or received in V3. Reduces enum clutter and cognitive overhead.

If you approve recommendations 1–5 as-is, Phase 1 can start.

---

## Execution sequence (suggested commit boundaries)

Each phase 1 commit should be independently revertable and pass `idf.py build`:

- **Commit 1:** Wire.h V4 schema (struct, enums, flag bits, opcode numbering). Plus `static_assert` checks for sizes. *Will not compile until commit 2 because the .cpp still references V3 symbols.*
  - Actually, better: combine commits 1+2 into one atomic "wire format bumped" commit so the tree always compiles.
- **Commit 1 (atomic):** Wire.h rewrite + bulk rename in System_ESPNow.cpp + callers. Compile-clean. No behavior change beyond "V3 frames are now rejected."
- **Commit 2:** `ctx.ip = "espnow:<mac>"` in `v3_handle_cmd` body. Two-line change. Compile-clean.
- **Commit 3:** Drop CMD_RESP 2KB cap, stream output through STREAM_FRAME during execution. New CMD_RESP_STATUS opcode + payload struct. Audit-pass on output paths. Compile-clean. **Biggest commit.**
- **Commit 4:** Cleanup — remove dead V3 opcodes, update docs, any leftover references.

---

## What's NOT in Phase 1

To keep scope tight, these explicitly wait for later phases:

- Multi-mesh data model — Phase 2. The header field exists; populating it from a multi-mesh `gSettings.meshes[]` array is Phase 2's work.
- Per-peer crypto / sessions — Phase 3. The header fields exist; the handshake opcodes, session state, and AEAD path are Phase 3.
- Per-peer file transfer slots — Phase 4.
- Event subscription registry — Phase 5.
- UX (web + CLI for new features) — Phase 6.

Phase 1 is purely infrastructure: get V4 on the wire, fix the two known bugs (ctx.ip + CMD_RESP cap), don't change protocol semantics elsewhere.

---

## After Phase 1 lands

- Both devices in your fleet reflash to the V4 firmware.
- Run the verification checklist above.
- All current functionality (bond, mesh, remote CLI, file transfer, sensor streaming) should work identically except remote CLI output no longer truncates at 2 KB.
- Phase 2 (multi-mesh data model) can start on top.
