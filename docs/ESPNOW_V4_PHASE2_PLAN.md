# ESPNOW V4 — Phase 2 Implementation Plan (Multi-Mesh)

**Parent plan:** [docs/ESPNOW_V4_PLAN.md](ESPNOW_V4_PLAN.md)
**Previous phase:** [docs/ESPNOW_V4_PHASE1_PLAN.md](ESPNOW_V4_PHASE1_PLAN.md) (✅ landed in commit `c6157f2`)
**Phase 2.1–2.4 status:** ✅ landed in commit `2fdec6b`
**Phase 2.5–2.8 status:** 🟡 pending — this document describes them in detail
**Target end state:** A device can participate in up to `N_MESHES = 4` independent meshes simultaneously, with per-mesh pairing, bond mode, and passphrase. Mesh isolation is enforced at the wire layer; multi-mesh is exposed via CLI + web UX.

---

## How to use this document

This is a **resumption doc** for a future agent or session. It assumes:
1. You have read access to the codebase at `/Users/morgan/esp/hardwareone-idf`.
2. You have NOT participated in the prior Phase 2 design conversation.
3. You're being asked to complete Phase 2.5–2.8 (or some subset).

Read top-to-bottom on first encounter. The §Design Decisions section captures *why* each choice was made — don't relitigate those without a strong reason. The §Current State section tells you exactly what exists in the tree as of commit `2fdec6b`. The §Remaining Work section is your task list; each sub-phase has file:line citations, code shapes, and verification steps.

Open questions are in §Open Questions; ask the user before assuming an answer.

---

## TL;DR

A device used to belong to **one** mesh, identified implicitly by `gSettings.espnowPassphrase`. Phase 2 turns this into a small array `gSettings.meshes[N_MESHES]` so the same device can simultaneously be in (e.g.) a "home" mesh and a "work" mesh, with distinct passphrases and peer sets per mesh. The V4 wire format already carries a `meshFingerprint` field in the header (added in Phase 1); Phase 2 populates and validates it.

**Done** (commit `2fdec6b`):
- Schema: `Settings::MeshIdentity`, `Settings::N_MESHES = 4`, `gSettings.meshes[]`, per-mesh bond arrays
- `EspNowDevice.meshId` — every paired peer tagged with which mesh
- TX path stamps `meshFingerprint` from peer's mesh
- RX path validates `meshFingerprint`; silent drop on mismatch
- Topology responses filtered by requester's mesh
- Init shim populates `meshes[0]` from legacy single-mesh fields

**Remaining** (Phase 2.5–2.8, ~1 day total):
- 2.5: `addPeer` / `espnowpair` accept `[mesh]` arg (30 min)
- 2.6: Bond consumer task iterates per-mesh (2-3 hours)
- 2.7: Sweep ~120 legacy-field callsites; remove legacy fields (half day)
- 2.8: `espnowmeshes` CLI command + web UI (2 hours)

After Phase 2 ships, **Phase 3** (Signed Ephemeral DH per-peer crypto) sits naturally on top: per-pair keys are scoped per-mesh because peers are already per-mesh.

---

## Why multi-mesh

A user's device collection isn't always one homogeneous fleet. Real scenarios:

- **Home + Work**: A device that's "yours" needs a different mesh at home (with your home peers) vs. at work (with shared work peers).
- **Test/dev + Prod**: Dev peers shouldn't pollute the production mesh and vice versa.
- **Guest meshes**: A temporary mesh for a visiting device that shouldn't see your private peers.
- **Tenant isolation**: One physical device, multiple logical owners with strict peer-set separation.

Without multi-mesh, the user picks ONE passphrase and joins ONE community. Multi-mesh removes that constraint. The cost is real but bounded: each additional mesh is ~80 bytes of NVS storage + a CRC16 fingerprint and a few flags in RAM.

The design goal isn't "many meshes" — it's "two or three meshes that don't interfere with each other." `N_MESHES = 4` is the cap. Beyond that, edit the constant and grow proportionally.

---

## Locked design decisions (rationale included)

These are decisions made during the Phase 2 design conversation. They are **locked** — don't reopen without strong reason. If you do reopen, read the rationale here first.

### D1. `N_MESHES = 4`

**Decision:** Cap at 4 simultaneous mesh memberships per device.

**Rationale:** Static array → no dynamic allocation, predictable RAM footprint. 4 covers (home + work + lab + spare) which is more than realistic use. Storage cost: ~80 bytes per mesh slot in NVS = 320 bytes total. RAM cost: same shape in `gSettings.meshes[]`. Going higher costs proportionally; going lower wouldn't free meaningful resources.

If a user actually needs 5+ meshes, this is a `#define` change in `System_Settings.h` and a rebuild — not a wire format break.

**Source:** [System_Settings.h:658](../components/hardwareone/System_Settings.h)

### D2. `meshFingerprint` = CRC16-CCITT(label)

**Decision:** Each mesh has a human-readable label (e.g. `"primary"`, `"work"`). The on-wire identifier is CRC16-CCITT of the label, occupying 2 bytes of the V4 header.

**Rationale:**
- **Why a hash, not the label?** Labels can be arbitrarily long; V4 header has limited room. 2 bytes is plenty for distinguishing N=4 meshes (birthday collision starts at ~256 distinct labels).
- **Why a hash, not a local index?** Each device has its own local index for each mesh, so indices aren't comparable between devices. Both devices computing `CRC16("home")` agree on the same fingerprint regardless of local indexing.
- **Why CRC16, not SHA?** This is a non-cryptographic identifier — authentication of mesh membership comes via the group key in Phase 3. CRC16 is already in the codebase (used for payload CRC) and is microsecond-fast. SHA-256-truncated would be overkill for ~4 distinct values.

**Source:** [System_ESPNow.cpp:1106 `meshFingerprintForLabel()`](../components/hardwareone/System_ESPNow.cpp)

### D3. Heartbeats: per-peer fan-out with per-peer mesh stamp

**Decision:** When emitting a HEARTBEAT, fan out to each known peer via `v4_send_frame`; each per-peer send stamps that peer's mesh fingerprint in the header. Not "one broadcast frame per mesh."

**Rationale:**
- Existing `v4_broadcast()` iterates `gMeshPeers[]` already, calling `v4_send_frame()` per peer.
- `v4_send_frame()` now (Phase 2.2) stamps fingerprint from peer's mesh via `fingerprintForPeer(dst)`.
- This means heartbeats arrive at each peer correctly mesh-scoped without any new heartbeat emission code.
- **Trade-off:** A device in mesh A with 10 peers in mesh A sends 10 HEARTBEAT frames per tick instead of 1. Marginal bandwidth cost; bigger gain is *zero new code*.
- **Future optimization:** True MAC-broadcast (`FF:FF:FF:FF:FF:FF`) with one frame per mesh would reduce traffic. Defer until measured to matter.

**Source:** This happens as a side effect of [System_ESPNow.cpp:1241 `h.meshFingerprint = fingerprintForPeer(dst)`](../components/hardwareone/System_ESPNow.cpp).

### D4. Out-of-mesh frames: silent drop

**Decision:** If an RX frame's `meshFingerprint` doesn't match any of our enabled meshes, drop it. Don't log. Don't ACK. Don't track.

**Rationale:**
- A device in a public/crowded RF environment may see neighboring fleets' frames. If we logged every drop, the log would be useless noise.
- Silent drop is also a privacy property: a frame from mesh "secret-project" arriving at a device in mesh "home" doesn't reveal that "secret-project" exists or that the home device noticed.
- Operational implication: when debugging "why isn't this peer talking to me?" the answer "the fingerprints don't match" is invisible from logs alone. Diagnosis tool: enable a verbose debug flag (Phase 2.x or Phase 6 work) to log dropped frames temporarily.

**Source:** [System_ESPNow.cpp:3034 RX validation block](../components/hardwareone/System_ESPNow.cpp)

### D5. `fingerprint = 0` is "no mesh scope" — accepted

**Decision:** A frame with `meshFingerprint = 0` is treated as transitional/pre-mesh and accepted by any receiver, regardless of mesh membership.

**Rationale:**
- First-boot devices haven't yet configured a mesh. They send frames with `meshFingerprint = 0` (the default of a zero-initialized header).
- Future handshake/discovery frames (Phase 3 KEY_EX_HELLO before mesh agreement) may legitimately have no mesh scope.
- Cost: a Phase-1-only device on the network won't be filtered out. Acceptable in the wipe-and-reflash model where Phase 2 ships fleet-wide simultaneously.

**Future tightening:** Phase 3 introduces the `HANDSHAKE` flag to mark frames that legitimately bypass mesh scope. Once that's in, `fingerprint=0 + no HANDSHAKE flag` could become a drop. Don't do this until Phase 3's handshake opcodes are widely used.

**Source:** [System_ESPNow.cpp:3033 RX validation: `if (h->meshFingerprint != 0 && meshByFingerprint(h->meshFingerprint) == nullptr) return true;`](../components/hardwareone/System_ESPNow.cpp)

### D6. Default mesh resolution: `isDefault` flag, fallback to first enabled

**Decision:** For TX where the destination is unknown (broadcast to all, or pre-pairing discovery), use the mesh flagged `isDefault = true`. If no mesh is so flagged, fall back to the first `enabled = true` mesh.

**Rationale:**
- The flag means: "of my meshes, which one is my 'home' for unsolicited outbound traffic?"
- User-configurable via CLI (Phase 2.8): `espnowmeshes setdefault <label>`.
- Init shim sets `meshes[0].isDefault = true` so single-mesh devices behave identically to Phase 1.

**Source:** [System_ESPNow.cpp:1099 `fingerprintForPeer()`](../components/hardwareone/System_ESPNow.cpp)

### D7. Per-mesh bond state

**Decision:** Each mesh can have its own independent bond pair. `gSettings.bondPeerMacMesh[N_MESHES]`, `bondRoleMesh[N_MESHES]`, `bondModeEnabledMesh[N_MESHES]`.

**Rationale:**
- A device in 2 meshes can have a bond with peer X in mesh A AND a separate bond with peer Y in mesh B. Each bond is its own state machine.
- The runtime cost is small (per-mesh arrays of small types).
- Phase 2.6 wires the bond consumer task to iterate.

**Source:** [System_Settings.h:675-677](../components/hardwareone/System_Settings.h)

### D8. Init shim mirrors legacy single-mesh fields into `meshes[0]`

**Decision:** On `initEspNow()`, if `meshes[0].label` is empty and `gSettings.espnowPassphrase` is non-empty, populate `meshes[0]` from the legacy fields. Phase 2.7 removes this shim when no more readers reference the legacy fields.

**Rationale:**
- Lets existing CLI commands (`espnowsetpassphrase`, `bondconnect`) keep writing the legacy fields during Phase 2.1–2.6.
- Each Phase 2.x sub-commit can migrate readers independently without forcing a 120-callsite single-commit change.
- The shim is dead code as soon as `gSettings.espnowPassphrase` is no longer written by anyone (Phase 2.7).

**Source:** [System_ESPNow.cpp:1158 `initPrimaryMeshFromLegacySettings()`](../components/hardwareone/System_ESPNow.cpp), called from [System_ESPNow.cpp:6435](../components/hardwareone/System_ESPNow.cpp)

### D9. No backwards compatibility

**Decision:** User explicitly confirmed: when a Phase 2 firmware is flashed, all devices in the fleet are wiped and reflashed simultaneously. No NVS migration code. No transitional behavior beyond the init shim mentioned above.

**Rationale:** Project direction. Greatly simplifies Phase 2 — no need to handle "device on old firmware sending frames our new firmware can't parse" cases.

### D10. `gMeshPeers[]` does NOT yet have a `meshId` field

**Decision:** Phase 2.4 topology filter uses `gEspNow->devices[].meshId` for *paired* peers, and falls back to "default mesh" for peers known only through mesh routing.

**Rationale:** Adding `meshId` to `gMeshPeers[]` is a separate refactor (Phase 2.4.1 or roll into 2.6) that wasn't critical to land 2.4. The current behavior is correct for typical cases (paired peers are accurate; routing-only peers default to primary mesh).

**Future:** When `gMeshPeers[]` learns about meshes, the fallback path in `peerIsInRequesterMesh` (defined inside `v4h_topo_req`) goes away.

---

## Architecture: the three layers

Multi-mesh touches three layers. Knowing which layer you're in clarifies what changes need to happen where.

### Layer 1: Wire format (Phase 1)

Already landed in V4. The `EspNowV4Header.meshFingerprint` (`uint16_t`, offset 22) is the only wire-level concept. It's stamped by the sender, validated by the receiver, never modified on the path.

No further wire format changes needed for Phase 2 or beyond.

### Layer 2: Data model (Phase 2)

Where mesh membership *lives*. Two structures:

- `Settings::MeshIdentity` — describes a mesh: label, passphrase, fingerprint, flags. Array of these in `gSettings.meshes[N_MESHES]`.
- `EspNowDevice.meshId` — tags each paired peer with which mesh it belongs to (index into `gSettings.meshes[]`).

The data model is in place after `2fdec6b`. Phase 2.x sub-phases mostly migrate *consumers* of this data, not the data model itself.

### Layer 3: User experience (Phase 2.5 + Phase 2.8 + Phase 6)

How users interact with meshes. CLI commands (`espnowpair [mesh]`, `espnowmeshes`, `bondconnect [mesh]`), web pages (`/espnow/meshes`, mesh selector on `/bond`), OLED displays (mesh selector during pairing wizard).

This layer is where Phase 2.5 and 2.8 do most of their work, with Phase 6 adding polish.

---

## Current state (as of commit `2fdec6b`)

This section enumerates exactly what exists in the codebase. A future agent should NOT reimplement these.

### Types defined

In [System_Settings.h](../components/hardwareone/System_Settings.h):
```cpp
class Settings {
public:
  static constexpr uint8_t N_MESHES = 4;

  struct MeshIdentity {
    String   label;
    String   passphrase;
    uint16_t fingerprint;   // CRC16-CCITT of label
    bool     enabled;
    bool     isDefault;
    MeshIdentity() : label(""), passphrase(""), fingerprint(0),
                     enabled(false), isDefault(false) {}
  };

  MeshIdentity meshes[N_MESHES];

  // Per-mesh bond arrays (alongside legacy singletons during Phase 2.x)
  bool    bondModeEnabledMesh[N_MESHES] = {};
  uint8_t bondRoleMesh[N_MESHES] = {};
  String  bondPeerMacMesh[N_MESHES];

  // Legacy fields — still read by ~120 callsites; Phase 2.7 will remove
  bool    bondModeEnabled;
  uint8_t bondRole;
  String  bondPeerMac;
  String  espnowPassphrase;
  // ... other fields ...
};
```

In [System_ESPNow.h](../components/hardwareone/System_ESPNow.h):
```cpp
struct EspNowDevice {
  uint8_t mac[6];
  String name;
  bool encrypted;
  uint8_t key[16];
  String friendlyName, room, zone, tags;
  bool stationary;
  uint8_t meshId;  // Phase 2 — defaults to 0 (primary mesh)
};
```

### Functions implemented

In [System_ESPNow.cpp](../components/hardwareone/System_ESPNow.cpp):

| Function | Location | Purpose |
|---|---|---|
| `meshFingerprintForLabel(String)` | ~line 1106 | Compute CRC16-CCITT of a label. Returns 0 for empty label. |
| `recomputeAllMeshFingerprints()` | ~line 1115 | Iterate `gSettings.meshes[]` and refresh each `.fingerprint` field. Cheap. |
| `meshByFingerprint(uint16_t)` | ~line 1124 | RX lookup. Returns `MeshIdentity*` or `nullptr` if not enabled / not found. |
| `meshByLabel(String)` | ~line 1136 | Find an enabled mesh by label. |
| `fingerprintForPeer(uint8_t* mac)` | ~line 1145 | TX lookup. Resolves dst MAC → peer record → meshId → fingerprint. Falls back to default mesh for unknown peers / broadcast. |
| `initPrimaryMeshFromLegacySettings()` | ~line 1180 | Init shim. Copies legacy `espnowPassphrase`/`bondPeerMac` into `meshes[0]` on first boot. |

### Wire-layer integration

TX sites (all now stamp `h.meshFingerprint`):

| Function | Line | How |
|---|---|---|
| `v4_send_frame` | ~1240 | `h.meshFingerprint = fingerprintForPeer(dst)` |
| `v4_send_chunked` (per-fragment header) | ~1392 | Same |
| `v4_send_ack` | ~1485 | Inherits via `v4_send_frame` call |
| `v4_send_frag_ack` | ~1512 | `h.meshFingerprint = fingerprintForPeer(dst)` (direct stamp) |
| `v4_broadcast` | ~1311 | Inherits via per-peer `v4_send_frame` fan-out |

RX validation, in [System_ESPNow.cpp:3033](../components/hardwareone/System_ESPNow.cpp) (after CRC check, before fragmentation handling):

```cpp
if (h->meshFingerprint != 0 && meshByFingerprint(h->meshFingerprint) == nullptr) {
  return true;  // silent drop
}
```

### Topology filter

[`v4h_topo_req`](../components/hardwareone/System_ESPNow.cpp) (~line 2166) now uses the requester's `meshFingerprint` to filter the peer list. Inline lambda `peerIsInRequesterMesh(mac)` does the lookup:

- If `requesterFp == 0` → no filter (transitional)
- For paired peer (in `gEspNow->devices[]`) → match `device.meshId` against `requesterFp`
- For routing-only peer (in `gMeshPeers[]` but not `gEspNow->devices[]`) → fall back to default mesh

### What's NOT changed

- `addPeer()` signature — still `addPeer(mac, name)`; all existing peers get `meshId = 0` (Phase 2.5 fixes this)
- `bondconnect` CLI command — still single-mesh (Phase 2.6)
- `espnowsetpassphrase` CLI — still writes to legacy `gSettings.espnowPassphrase` (Phase 2.7)
- Bond consumer task loop — still iterates a single bond (Phase 2.6)
- Web `/espnow` page — single-mesh peer list (Phase 6 or Phase 2.8 light touch)
- Web `/bond` page — single-mesh bond display (Phase 2.6 or 2.8)

---

## Remaining work: Phase 2.5–2.8

### Phase 2.5 — `addPeer` and `espnowpair` accept `[mesh]` arg

**Goal:** Let users pair a peer into a specific mesh, not just the default.

**Effort:** ~30 minutes.

**Risk:** Low. Pure signature change with a default value; all existing callers unaffected.

**Files to touch:**
- `components/hardwareone/System_ESPNow.h` — `addPeer` declaration
- `components/hardwareone/System_ESPNow.cpp` — `addPeer` definition + the `espnowpair` / `espnowpairsecure` CLI handlers

**Concrete edits:**

1. **Signature change.** Find `addPeer` declaration in `System_ESPNow.h` (probably in the EspNowState section or near it). Change:
   ```cpp
   bool addPeer(const uint8_t* mac, const String& name);
   ```
   to:
   ```cpp
   bool addPeer(const uint8_t* mac, const String& name, uint8_t meshId = 0);
   ```

2. **Definition update.** Find `addPeer` definition in `System_ESPNow.cpp`. After the device record is created, set `device.meshId = meshId`. If the function returns the index or pointer to the new device, set the field there.

3. **CLI argument parsing.** Find `cmd_espnow_pair` and `cmd_espnow_pairsecure` (search for `"espnowpair"` and `"espnowpairsecure"` in the command registry). They likely use a `CommandArgs` helper. Add an optional last argument:
   - Numeric: parse as index `0..N_MESHES-1`, validate `gSettings.meshes[N].enabled`.
   - String: look up via `meshByLabel(arg)`, get its index.
   - Missing: default to mesh 0 (or the `isDefault` mesh; minor design choice — pick `isDefault` for consistency).

4. **Command help text.** Update the help string in the command registry:
   ```cpp
   { "espnowpair", "Pair a device: 'espnowpair <mac> [name] [mesh]'.", ... }
   ```

**Verification:**
- `espnowpair AA:BB:CC:DD:EE:FF` (no mesh) — should default to primary mesh; device's `meshId` = 0.
- `espnowpair AA:BB:CC:DD:EE:FF kitchen work` — should pair into mesh "work" if it exists; device's `meshId` = index of "work".
- `espnowpair AA:BB:CC:DD:EE:FF kitchen nonexistent` — should error out, not silently succeed.

### Phase 2.6 — Bond consumer task iterates per-mesh

**Goal:** Each mesh can have its own bond pair. The bond state machine runs once per active bond, not globally.

**Effort:** 2-3 hours.

**Risk:** Medium. Bond logic is one of the more complex parts of the codebase; per-mesh-ification touches multiple state machines.

**Files to touch:**
- `components/hardwareone/System_ESPNow.cpp` — bond consumer task block (around line 5950–6200)
- `components/hardwareone/System_ESPNow.h` — `EspNowState` may need per-mesh bond runtime fields
- `components/hardwareone/WebPage_Bond.cpp` — endpoints may need per-mesh awareness (small scope: Phase 6 will polish this further)

**Key insight about scope:** Many `gEspNow->bond*` runtime fields (e.g., `bondPeerOnline`, `bondPeerBootCounter`, `bondRssiAvg`) are currently single-valued. For full per-mesh support, these become arrays indexed by mesh. Alternatively, Phase 2.6 can limit each device to "at most one active bond at a time" (across all meshes) — the simpler model — and tackle per-mesh-bond-simultaneity in a later phase.

**Recommended approach: simpler model first.**

- Treat the per-mesh `gSettings.bondModeEnabledMesh[]` as defining which mesh(es) bond is configured for, but require that at most ONE bond is *active* at a time. The "active mesh" is determined by which mesh has `bondModeEnabledMesh[i] == true` *and* has the most recent bond activity.
- This avoids needing per-mesh `EspNowState` runtime fields (huge refactor).
- Users with multiple bonded meshes manually switch via CLI.

**Concrete edits (simpler model):**

1. **Bond consumer task scan.** In the consumer block (line ~5950 onwards), wherever the code reads `gSettings.bondModeEnabled` or `gSettings.bondPeerMac`, change to:
   ```cpp
   uint8_t activeMesh = findActiveBondMesh();  // helper that picks the mesh with bondModeEnabledMesh[i]==true
   if (activeMesh == 0xFF) { /* no bonds active */ return; }
   const String& peerMac = gSettings.bondPeerMacMesh[activeMesh];
   uint8_t role = gSettings.bondRoleMesh[activeMesh];
   // ... rest of bond logic
   ```

2. **Helper function.** Add:
   ```cpp
   static uint8_t findActiveBondMesh() {
     for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
       if (gSettings.bondModeEnabledMesh[i] && gSettings.bondPeerMacMesh[i].length() > 0) {
         return i;
       }
     }
     return 0xFF;
   }
   ```

3. **`bondconnect` CLI.** Accepts optional `[mesh]` arg. Writes to `bondPeerMacMesh[meshId]`, `bondRoleMesh[meshId]`, `bondModeEnabledMesh[meshId]` (instead of legacy singletons).

4. **`bonddisconnect` CLI.** Accepts optional `[mesh]` arg. Defaults to "the currently active bond." Sets `bondModeEnabledMesh[meshId] = false`.

5. **`bondstatus` CLI.** Reports status for the active bond (or all bonded meshes if explicit `bondstatus all`).

**Better approach: full per-mesh** (more code, more correctness):

Add per-mesh runtime arrays in `EspNowState`:
```cpp
struct BondRuntimeState {
  bool peerOnline;
  uint32_t peerBootCounter;
  uint32_t peerSettingsHash;
  uint32_t peerUptime;
  bool capSent;
  // ... etc.
};
BondRuntimeState bondRuntime[Settings::N_MESHES];
```

Then every `gEspNow->bond*` reference becomes `gEspNow->bondRuntime[meshId].*`. This is the correct long-term design but ~2× the code change of the simpler model.

**Recommendation:** **Ship simpler model in Phase 2.6.** Note in code comments that full per-mesh bond runtime is deferred. Real-world devices rarely have multiple simultaneously-active bonds; the simpler model covers ~99% of usage.

**Verification:**
- Configure mesh A with bond peer X. `bondstatus` shows bond active for mesh A.
- Configure mesh B (no bond). `bondstatus` unchanged.
- `bondconnect Y work` (with mesh B = "work") — mesh B now has bond config.
- Run with one mesh's bond at a time, swap, verify state machine recovers cleanly.

### Phase 2.7 — Sweep ~120 legacy-field callsites; remove legacy fields

**Goal:** Eliminate the parallel data paths. All code reads from `gSettings.meshes[]` / `gSettings.bondPeerMacMesh[]`; the legacy `espnowPassphrase` / `bondPeerMac` / `bondRole` / `bondModeEnabled` fields are deleted from the struct.

**Effort:** Half day.

**Risk:** Medium-high. ~120 callsites to update. Compile errors will surface any missed sites. Some callsites need contextual decisions about WHICH mesh they refer to.

**Files to touch (in approximate order of complexity):**

| File | Approx ref count | Notes |
|---|---|---|
| `components/hardwareone/System_ESPNow.cpp` | ~77 | Largest. Most refs are bond-related; many auto-fix after Phase 2.6 lands. |
| `components/hardwareone/OLED_Mode_Remote.cpp` | 6 | Likely OLED bond status displays |
| `components/hardwareone/OLED_Mode_UnifiedMenu.cpp` | 6 | OLED menu items |
| `components/hardwareone/OLED_RemoteSettings.cpp` | 5 | OLED settings pages |
| `components/hardwareone/WebPage_Bond.cpp` | 5 | Web bond page |
| `components/hardwareone/OLED_Utils.cpp` | 4 | Shared OLED helpers |
| `components/hardwareone/WebServer_Server.cpp` | 4 | Web routing/auth |
| `components/hardwareone/System_ESPNow.h` | 4 | Declarations |
| `components/hardwareone/System_ESPNow_Sensors.cpp` | 3 | Sensor sync paths |
| `components/hardwareone/OLED_Mode_Network.cpp` | 2 | OLED network status |
| `components/hardwareone/OLED_ESPNow.cpp` | 1 | OLED ESPNow main mode |
| `components/hardwareone/System_Utils.cpp` | 1 | Settings serialization probably |

**Strategy:**

1. **Run the grep:**
   ```
   grep -rnE "gSettings\.(espnowPassphrase|bondPeerMac|bondRole|bondModeEnabled)\b" components/hardwareone/
   ```

2. **Classify each callsite** into one of three categories:
   - **A. Single-mesh-meaning (use `meshes[0]`).** The code logically operates on the "primary" mesh — e.g., a settings display that shows "your passphrase." Replace `gSettings.espnowPassphrase` with `gSettings.meshes[0].passphrase`.
   - **B. Per-peer-meaning (use peer's mesh).** The code operates on a specific peer — e.g., "send a packet to peer X." Look up the peer, get their `meshId`, use `gSettings.meshes[meshId].passphrase`.
   - **C. Per-bond-meaning (use active bond's mesh).** Use `findActiveBondMesh()` from Phase 2.6, then index into the per-mesh array.

3. **Edit each callsite.** Mechanical for category A; needs thought for B and C.

4. **Once all callsites are updated, delete the legacy fields** from `System_Settings.h`:
   ```cpp
   // Remove:
   bool bondModeEnabled;
   uint8_t bondRole;
   String bondPeerMac;
   String espnowPassphrase;
   ```

5. **Remove `initPrimaryMeshFromLegacySettings()`** from `System_ESPNow.cpp` — it's dead code once the legacy fields are gone.

6. **Update CLI commands** that wrote to the legacy fields:
   - `espnowsetpassphrase <passphrase>` — change semantics. Either:
     - (a) Become `espnowsetpassphrase <passphrase>` = set primary mesh's passphrase, OR
     - (b) Deprecate in favor of `espnowmeshes setpassphrase <label> <passphrase>` from Phase 2.8.
     - Recommendation: (a) for backward usability + (b) as the proper new API. Keep both.

**Verification:**
- Compile-clean after each batch of callsite edits.
- After the sweep, fresh-flash a device and configure a mesh via the new flow. Confirm pairing, bond, remote CLI all work.
- A device with `meshes[0]` configured (primary mesh "primary" with some passphrase) should behave identically to a Phase 2.6 device with the same legacy passphrase.

**Footgun:** Some sites read `gSettings.espnowPassphrase` as a *boolean check* — `if (gSettings.espnowPassphrase.length() > 0)` ("is encryption enabled?"). The migrated check needs to be "is ANY mesh configured?" Choose the right replacement carefully — this is category A logic but the meaning shifts subtly.

### Phase 2.8 — CLI command + light web UI

**Goal:** Users can configure multiple meshes via CLI.

**Effort:** ~2 hours.

**Risk:** Low. Pure new code; doesn't touch existing flow.

**Files to touch:**
- `components/hardwareone/System_ESPNow.cpp` — new CLI handlers + registry entries

**CLI commands to implement:**

```
espnowmeshes                           # list all configured meshes
espnowmeshes add <label> <passphrase>  # add a new mesh
espnowmeshes remove <label>            # remove a mesh; unpair all its peers
espnowmeshes setdefault <label>        # mark a mesh as the isDefault
espnowmeshes setpassphrase <label> <passphrase>  # change a mesh's passphrase
espnowmeshes rename <oldlabel> <newlabel>        # rename (recomputes fingerprint!)
```

**Pseudocode shapes:**

```cpp
const char* cmd_espnowmeshes(const String& argsInput) {
  CommandArgs a(argsInput);
  if (a.count() == 0) return cmd_espnowmeshes_list();
  String sub = a.arg(0);
  if (sub == "add") return cmd_espnowmeshes_add(a.arg(1), a.arg(2));
  if (sub == "remove") return cmd_espnowmeshes_remove(a.arg(1));
  if (sub == "setdefault") return cmd_espnowmeshes_setdefault(a.arg(1));
  if (sub == "setpassphrase") return cmd_espnowmeshes_setpass(a.arg(1), a.arg(2));
  if (sub == "rename") return cmd_espnowmeshes_rename(a.arg(1), a.arg(2));
  return "Usage: espnowmeshes [list|add|remove|setdefault|setpassphrase|rename] ...";
}
```

**Key implementation rules:**
- `add`: find first free slot (where `enabled == false`). Set label, passphrase, fingerprint = CRC16(label), enabled = true. If `meshes[]` is full, return error.
- `remove`: find by label. Disable. Unpair all `gEspNow->devices[]` entries with matching `meshId` (set the device to "unpaired" or remove from list). Call `recomputeAllMeshFingerprints()` if needed.
- `rename`: find by label, change `.label`, recompute `.fingerprint`. **Footgun:** changing the fingerprint orphans all existing on-the-wire references — peers in this mesh stop receiving until they too rename. Warn user explicitly.
- `setdefault`: clear `isDefault` on all meshes, then set the target.

**Label validation:**
- Non-empty, max ~16 chars, ASCII printable, no whitespace. Reject reserved labels like "internal", "system", "all".
- Reject if collision with an existing label.

**Persistence:** `setSetting` calls on each modified `MeshIdentity` field, OR a single `saveSettings()` after the batch. Settings serialization needs to be aware of the array — verify `System_Settings.cpp` or wherever JSON ser/de happens.

**Verification:**
- Add mesh "work", confirm `meshes[1]` populated.
- Pair a peer into "work" (uses Phase 2.5's CLI extension).
- Remove mesh "work", confirm peer is no longer paired.
- Rename mesh "primary" to "home", confirm fingerprint changes and existing peers in the mesh need re-pairing.

**Web UI (optional within Phase 2.8):** A simple `/espnow/meshes` page that lists configured meshes with edit/remove buttons. Out of scope for Phase 2; defer to Phase 6 UX polish.

---

## Cross-cutting concerns

These don't belong to any single sub-phase but affect multiple.

### CC1. ESPNOW radio-layer peer table

ESP-IDF's `esp_now_peer_info_t` carries a Local Master Key (LMK) per peer for radio-layer AES-128 encryption. In Phase 2, every peer's LMK still derives from a single global passphrase — there's no per-mesh LMK isolation yet.

This is **acceptable** for Phase 2 because:
- Radio-layer encryption is going away in Phase 3 (replaced by AEAD session keys).
- Single LMK across meshes means a sniffer in mesh A can technically decrypt mesh B's broadcasts. Not great, but radio-layer encryption was never the strong protection layer.

**Action:** Document this limitation. Defer to Phase 3 where per-pair AEAD keys derived from per-mesh group keys solve it correctly.

### CC2. Persistence: settings JSON serialization

`gSettings` is persisted as JSON. The new `meshes[N_MESHES]` array needs to serialize/deserialize correctly. Inspect `System_Settings.cpp` (or wherever `saveSettings` / `loadSettings` live) and add:

```cpp
// On save:
JsonArray meshesArr = doc.createNestedArray("meshes");
for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
  JsonObject m = meshesArr.createNestedObject();
  m["label"]      = gSettings.meshes[i].label;
  m["passphrase"] = gSettings.meshes[i].passphrase;
  m["enabled"]    = gSettings.meshes[i].enabled;
  m["isDefault"]  = gSettings.meshes[i].isDefault;
  // Note: don't serialize .fingerprint — it's derived from .label,
  // recomputed by recomputeAllMeshFingerprints() at load time.
}
// Same shape for bondModeEnabledMesh, bondRoleMesh, bondPeerMacMesh arrays.

// On load: reverse, then call recomputeAllMeshFingerprints().
```

**Footgun:** the legacy `espnowPassphrase` / `bondPeerMac` etc. still serialize during Phase 2.x to support the init shim. After Phase 2.7 removes them from the struct, also remove from the serializer. Don't leave dead serialization code.

### CC3. Mesh label collisions

Two meshes can't share a label. The CLI `espnowmeshes add` should reject duplicates. But what about same-fingerprint different-label collisions? With CRC16 the birthday collision rate is 1/65536 — possible but vanishingly unlikely with human-chosen short labels. Don't engineer for it; if it happens, user renames one.

If you want hard collision protection: when adding a mesh, compute fingerprint and check no existing mesh has the same fingerprint. Reject with "label collides with existing mesh; try a different one." Adds 4 lines.

### CC4. Bond UI surfaces

Three places display bond state today:
- Web `/bond` page (`WebPage_Bond.cpp`)
- OLED bond status in unified menu (`OLED_Mode_UnifiedMenu.cpp`)
- CLI `bondstatus` command

All three currently read singleton `gSettings.bondModeEnabled` / `gSettings.bondPeerMac`. Phase 2.6's "simpler model" lets them read the same way (just substitute `findActiveBondMesh()` lookup), so they don't need major refactoring. Phase 6 polish would let them iterate all meshes and show per-mesh status.

### CC5. The init shim ordering

`initPrimaryMeshFromLegacySettings()` runs at the top of `initEspNow()`. If the user changes `gSettings.espnowPassphrase` *after* boot (via `espnowsetpassphrase`), the shim has already run and won't repopulate `meshes[0]`. The CLI command must explicitly update both fields during the transition (Phase 2.x).

**Phase 2.7's job:** remove this footgun by making CLI commands write to `meshes[]` directly, removing the shim.

---

## Footguns and gotchas (collected list)

1. **`gMeshPeers[]` has no `meshId`.** Routing-layer peers fall back to default mesh in topology filter. Fix when full per-mesh routing is needed.
2. **CLI passphrase change doesn't auto-mirror to `meshes[]`.** Until Phase 2.7, the init shim only runs at boot — runtime passphrase changes via CLI must explicitly update both fields.
3. **Mesh rename = fingerprint change = peers stop receiving.** Renames are intentional break-glass operations.
4. **Mesh removal doesn't auto-unpair peers** unless explicitly coded. `espnowmeshes remove` should cascade.
5. **`gSettings.bondPeerMac` boolean checks**: `if (gSettings.bondPeerMac.length() > 0)` becomes `if (findActiveBondMesh() != 0xFF)` — different meaning, watch each site.
6. **Settings JSON backwards compat across the cutover:** during Phase 2.x while legacy fields exist, serialize both. After Phase 2.7, only the new fields.
7. **Heartbeat to peer in unknown mesh:** `fingerprintForPeer` returns default mesh's fingerprint. The peer rejects (their mesh doesn't match). Fine, but watch in logs — bondsync may show "peer offline" until you realize the peer's mesh isn't yours.
8. **Reserved labels:** consider rejecting "system", "internal", "all", "default" as user labels. Avoids confusion in CLI output.
9. **`espnowsetpassphrase` legacy command:** decide if it stays (writes to meshes[0]) or is removed in favor of `espnowmeshes setpassphrase`.
10. **First-time setup wizard** (`System_SetupWizardMode`): if it prompts for a passphrase, it should now prompt for "primary mesh passphrase" and stuff that into `meshes[0]`. Currently writes `gSettings.espnowPassphrase`.
11. **`espnowlist` output format:** should it include mesh column? With one mesh, no need. With multiple, yes. Decide based on N enabled meshes at runtime.
12. **Topology discovery range:** `meshtopo` command iterates known peers — should filter by current default mesh or accept a mesh arg.

---

## Verification approach

### Single-device tests

1. **Fresh boot, no config:** All `meshes[]` empty, ESPNOW frames go out with `fingerprint = 0`. Verify with serial debug log of a TX frame.
2. **Set legacy passphrase, reboot:** Init shim runs, `meshes[0].label == "primary"`, `meshes[0].fingerprint != 0`. Verify with `espnowmeshes` (Phase 2.8) or `espnowstatus`.
3. **Add second mesh via CLI:** `espnowmeshes add work foo123` — `meshes[1]` populated. Persists across reboot.
4. **Remove a mesh:** All peers with that `meshId` are unpaired.

### Two-device tests

1. **Both in same mesh:** Bond + manifest + sensor streaming all work as in Phase 1 (regression check).
2. **Devices in different meshes:** Device A in "home" only, Device B in "work" only — pairing them should fail (or be possible only if pairing crosses mesh boundaries, which is a design decision; recommend pairing is per-mesh only).
3. **Device A in {home, work}, Device B in {work}:** A↔B works only through the "work" mesh. Frames stamped with "home" arrive at B and get silently dropped.
4. **TOPO_REQ in mesh A on Device B (which is in {home, work}):** Returns only peers in mesh "home" (matching A's mesh).

### Three-device tests

1. **A in {home}, B in {home, work}, C in {work}:** A↔B works, B↔C works, A↔C silently dropped.
2. **Bond per-mesh:** B bonded with A in home, B bonded with C in work. Two independent bond sync flows complete.

### Performance verification

1. **Heartbeat traffic:** With N peers in M meshes, expect N HEARTBEAT frames per heartbeat tick (per-peer fan-out). Measure with serial log.
2. **TX overhead per frame:** `fingerprintForPeer()` lookup is O(devices). For ~10 devices, sub-microsecond cost. Negligible.

---

## Open questions

Things I'd ask the user before committing to a specific implementation. The Phase 2 design conversation didn't resolve these definitively.

1. **Cross-mesh pairing allowed?** Currently a peer belongs to exactly one mesh (`meshId` is scalar). What if a user wants peer X to be in BOTH home AND work meshes? Two options: (a) clone the peer record into both meshes (separate entries), (b) make `meshId` a bitmap allowing multi-mesh membership. Recommend (a) — simpler, more explicit, no edge cases in routing.
2. **Bond max one per mesh?** Allow each mesh to have its own bond pair — yes, that's Phase 2.6's design. But can a single device have multiple SIMULTANEOUSLY active bonds (one per mesh)? Currently the runtime state is global. Recommend "yes in config, no in runtime" for Phase 2.6 (the simpler model).
3. **Mesh deletion cascade:** When removing mesh "work", what happens to peers paired in mesh "work"? Three options: (a) unpair them automatically, (b) move them to the default mesh, (c) refuse to remove until peers are manually unpaired. Recommend (a) — explicit and simple.
4. **Default mesh choice on first boot:** Should fresh-device first-boot create `meshes[0] = "primary"` with empty passphrase (waiting for user)? Or leave all meshes empty until user runs `espnowmeshes add`? Recommend the latter — explicit setup, no "default state" surprise.
5. **Reserved mesh labels:** `system`, `internal`, `all`, `default`, `none`. Reject as user labels? Recommend yes, with friendly error message.
6. **Web UI scope in Phase 2.8 vs Phase 6:** Phase 2.8 ships only CLI; Phase 6 ships web UI. Alternatively, a thin `/espnow/meshes` HTML page in Phase 2.8 to make the feature usable from a browser. Recommend the thin page — it's ~50 lines of HTML and turns Phase 2 from "developer-only" to "user-facing."
7. **Persistence schema versioning:** Should `settings.json` carry a schema version number so future migrations don't need init shims? Recommend yes — add a top-level `"_schema": 2` field; load code checks and migrates.

---

## Interactions with future phases

### Phase 3 — Per-peer crypto (Signed Ephemeral DH)

Phase 3 builds directly on Phase 2's per-peer-with-meshId model:

- Each `EspNowDevice` gains long-term Ed25519 identity (~32 bytes pub key).
- Per-pair session keys are derived from the mesh's group key (which derives from the mesh's passphrase via PBKDF2). So session keys are *inherently* per-mesh.
- Sessions are stored per-peer; when the peer is removed from a mesh, the session goes with it.
- The `meshFingerprint` field stays in the V4 header; Phase 3 doesn't change the wire format meaningfully (adds the `SESSION_FRAME` flag and per-frame session/sequence IDs).

Phase 3 is significantly easier with Phase 2 complete than without it.

### Phase 4 — Per-peer file transfer slots

Independent of Phase 2. File transfers are peer-to-peer; mesh scoping happens at the wire layer (via fingerprint check) before file transfer logic ever runs.

### Phase 5 — Event subscription registry

Subscriptions are per-peer naturally. When listing subscribed peers, optionally group by mesh. No deep interaction.

### Phase 6 — UX polish

Phase 6 builds on Phase 2's CLI surface. Web pages for mesh management, OLED mesh selectors during pairing, etc. Phase 6 also includes the "fancy" UX features deferred from Phase 2.8 (richer web UI, mesh-color-coded peer lists, etc.).

---

## File reference catalog

Files that Phase 2 has touched or will touch:

| File | Phase | Reason |
|---|---|---|
| `components/hardwareone/System_Settings.h` | 2.1 ✅ | `MeshIdentity`, `N_MESHES`, `meshes[]`, per-mesh bond arrays |
| `components/hardwareone/System_ESPNow.h` | 2.1 ✅ + 2.5 | `EspNowDevice.meshId` (done) + `addPeer` signature change (pending) |
| `components/hardwareone/System_ESPNow.cpp` | 2.1–2.4 ✅ + 2.5–2.7 | Helpers, init shim, TX/RX integration, topology filter (done); CLI updates, sweep, bond per-mesh (pending) |
| `components/hardwareone/System_Settings.cpp` (if exists) | 2.7 | Settings JSON ser/de updates |
| `components/hardwareone/System_Utils.cpp` | 2.7 | One ref to legacy `gSettings.bondPeerMac` |
| `components/hardwareone/System_ESPNow_Sensors.cpp` | 2.7 | Three legacy refs |
| `components/hardwareone/OLED_Mode_Remote.cpp` | 2.7 | OLED bond status display |
| `components/hardwareone/OLED_Mode_UnifiedMenu.cpp` | 2.7 | OLED menu items |
| `components/hardwareone/OLED_RemoteSettings.cpp` | 2.7 | OLED settings pages |
| `components/hardwareone/OLED_Mode_Network.cpp` | 2.7 | OLED network status |
| `components/hardwareone/OLED_ESPNow.cpp` | 2.7 | OLED ESPNow mode |
| `components/hardwareone/OLED_Utils.cpp` | 2.7 | Shared OLED helpers |
| `components/hardwareone/WebPage_Bond.cpp` | 2.7 + 2.8 | Web bond page; eventually mesh selector |
| `components/hardwareone/WebServer_Server.cpp` | 2.7 | Web routing/auth |
| `components/hardwareone/System_FirstTimeSetup.cpp` | 2.7 (footgun #10) | Setup wizard passphrase prompt |

---

## Glossary

- **Mesh** — A logical grouping of devices identified by a shared label and passphrase. Devices in the same mesh can talk; devices in different meshes can't (frames silently dropped at the wire layer).
- **MeshIdentity** — The struct describing one mesh: label, passphrase, fingerprint, enabled/default flags. Lives in `gSettings.meshes[]`.
- **`meshFingerprint`** — The 16-bit CRC16-CCITT of a mesh's label. Stamped in the V4 frame header for routing/filtering. Stable across devices (everyone computes the same hash from the same label).
- **`meshId`** — A local-to-this-device index into `gSettings.meshes[]`. Different devices may use different `meshId` values for the same mesh — that's why `meshFingerprint` (not `meshId`) goes on the wire.
- **Default mesh** — The mesh flagged `isDefault = true`. Used for outbound frames to unknown destinations. There can be only one default at a time.
- **Active mesh** (Phase 2.6 context) — In the simpler bond model, the mesh whose `bondModeEnabledMesh[i] == true` is currently driving the global bond state machine. At most one at a time.
- **Init shim** — `initPrimaryMeshFromLegacySettings()`. Mirrors legacy `espnowPassphrase` / `bondPeerMac` into `meshes[0]` on boot. Dies in Phase 2.7 when no readers remain.
- **Legacy fields** — The singleton `gSettings.espnowPassphrase`, `bondPeerMac`, `bondRole`, `bondModeEnabled`. Still exist during Phase 2.x; removed in Phase 2.7.
- **`fingerprintForPeer(mac)`** — TX helper. Resolves a destination MAC to the correct mesh fingerprint by looking up the peer's `meshId` in `gEspNow->devices[]`, with default-mesh fallback.
- **`meshByFingerprint(fp)`** — RX helper. Returns the local `MeshIdentity*` for a given on-wire fingerprint, or `nullptr` if no enabled mesh matches.

---

## Suggested resumption order

If you're a fresh agent picking this up, suggested order:

1. **Read this entire document** before touching any code.
2. **Spot-check the current state:** run `git log --oneline | head -10` to confirm `2fdec6b` is the most recent commit and Phase 2.5+ haven't been started.
3. **Verify compile-clean:** `ninja -C build esp-idf/hardwareone/libhardwareone.a` should succeed before you start.
4. **Phase 2.5 first** — smallest, lowest risk, gets the CLI surface in place. Commit when done.
5. **Phase 2.8 second** — `espnowmeshes` CLI command. Without this, users can't actually create a second mesh, so 2.5's `[mesh]` arg has nothing to point at.
6. **Phase 2.6 next** — bond per-mesh with simpler model. Commit when done.
7. **Phase 2.7 last** — the big sweep + legacy removal. Largest change, do it when everything else is settled.

Each sub-phase should compile clean on its own, be testable, and be revertable. Don't combine sub-phases into one big commit.

After Phase 2.8 lands, the V4 plan's Phase 3 (per-peer crypto) is the next logical step.

---

## Closing notes

The Phase 2 design held up well through implementation of 2.1–2.4. No surprises. The remaining sub-phases are mostly mechanical with one judgment call (Phase 2.6's simpler-vs-full bond model) that has a clear recommended choice.

The main risk in 2.7 is missing a callsite — the build will catch syntactic mismatches but a logic-only swap (e.g., reading from the wrong mesh) will silently misbehave. Counter-strategy: after each batch of callsite edits, manually exercise the affected feature on hardware.

The remaining decision points are well-documented in §Open Questions. Ask the user on each before committing to an implementation choice. Most defaults are sensible; only #1 (cross-mesh pairing) and #2 (simultaneous bonds) are genuinely subjective.

Good luck.
