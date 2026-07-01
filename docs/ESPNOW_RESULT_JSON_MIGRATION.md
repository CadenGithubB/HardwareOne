# ESP-NOW result handling vs the OK:/Error: stamp ("lunchbox" plan)

Follow-up to [project_uniform_return_contract] (the `stampOkStatus` work). Original goal: stop the
`OK:` stamp from needing a hand-maintained machine-token allowlist (`commandIsTokenExempt`).

---

## ⚠ Update 2026-06-29 — investigation changed the plan

When I started this, two assumptions in the original plan turned out **false**, which split the work
into a cheap part (done) and an optional bigger part (re-scoped):

1. **`gMeshTopology` is dead.** The `std::vector<MeshTopoNode>` (`System_ESPNow.cpp:300`) is declared
   and exported but **never populated**. The real topology data flows as **pre-formatted text**:
   per-peer arrival (`V4_RX_TOPO_PEER` handler ~3120) stringifies `{name, mac, rssi}` straight into
   `stream->accumulatedData`, which `finalizeTopologyStream()` (~6627) concatenates into
   `gTopoResultsBuffer`. The raw peer struct is discarded on arrival. So there is **no structured
   store** to serialize — clean JSON would require *adding* structured accumulation into the topo
   streaming path (the delicate out-of-order / dedup / finalize code).
2. **The "machine tokens" have no consumer.** Grepping every reference:
   - `"WAIT"` (toporesults, collecting) — consumed by **nobody**. The web (`WebPage_ESPNow.h`
     `refreshTopologyView` ~2290 and the twin ~2698) only substring-checks `ERROR` / `No topology` /
     `not enabled`; it never tests `WAIT`. The only reference was the stamp exemption itself.
   - `"alive"/"rejected"/"no response (timeout)"` (espnowprobe) — a debug-buffer-unavailable
     **fallback**, display-only, no parser (confirmed in the earlier 5-dim audit).

   So the allowlist was protecting **phantoms**. Also: the toporesults **text table** survives the
   stamp fine — `"OK: "` lands on the line *before* the `=== … ===` header, and the web splits on
   `\n` and matches device-header lines, so the prefix matches no regex. And the no-results `"ERROR"`
   return is classified as a *failure* (starts with `ERROR`), so it is never stamped and the web's
   `indexOf('ERROR')` keeps working.

**Conclusion:** the allowlist could be deleted cheaply (Part A) without any JSON migration. The JSON
migration (Part B) is now a **bigger, optional robustness feature**, not a quick follow-up.

---

## Part A — Delete the allowlist (DONE, built green 2026-06-29)

- `System_ESPNow.cpp` espnowprobe fallback: `"no response (timeout)"/"alive"/"rejected"` →
  `"No response (timeout)"/"Peer is alive"/"Peer rejected the probe"` (prose; reads fine stamped).
- `System_ESPNow.cpp` espnowtoporesults: bare `"WAIT"` → `"Still collecting topology responses;
  run espnowtoporesults again shortly."` (prose). The two `"ERROR"` returns left as-is (failures the
  web detects by substring).
- `System_Utils.cpp` `stampOkStatus`: deleted `commandIsTokenExempt()`, the `strcmp(out,"WAIT")`
  line, and the `cmd` parameter; updated both chokepoint call sites. Remaining exemptions are all
  structural/general: success-gate, validate (`gCLIValidateOnly`), empty, JSON `{`/`[`, bare
  `OK`/`OK:`, `SUCCESS`. **The only thing protecting machine answers now is the JSON rule.**

Net: the hand-maintained list is **gone**; no new silent-break footgun.

---

## Part B — Topology JSON + web renderer (OPTIONAL, re-scoped — NOT a quick win)

Still worthwhile for agent-legibility (a structured mesh graph the openclaw agent / app can read) and
to kill the fragile ~80-line regex scraper in the web UI. But with `gMeshTopology` dead, it is a
**feature-sized change to delicate streaming code**, so treat it as its own project with its own HW
test. Decide explicitly before doing it.

**B1. Capture structured peers as they stream in** (`System_ESPNow.cpp`):
- Add a structured accumulator to `TopologyStream` (e.g. `std::vector<MeshTopoPeer> peers;` or a
  small `{mac,name,rssi}` vector) and populate it in the `V4_RX_TOPO_PEER` handler (~3164) **next to**
  the existing `accumulatedData += peerInfoBuf;`. Live per-peer fields available there: `tp->mac`,
  resolved `peerNamePtr2`, `tp->rssi`. (Heartbeats / lastSeen / `Path:` exist only in the test
  fixture at ~11010, not in live stream payloads — don't promise them in the JSON.)
- In `finalizeTopologyStream()` (~6627), push a `MeshTopoNode{senderMac, senderName, peers}` into
  `gMeshTopology` (finally populating it) alongside the text concat. Clear it where the text buffer
  is cleared (~7367, ~11000).

**B2. `espnowtoporesults` JSON branch** (`System_ESPNow.cpp:10886`):
- `if (argWantsJson(argsInput))` → serialize from `gMeshTopology`:
  ```json
  {"status":"collecting"}
  {"status":"none"}
  {"status":"ready","responses":N,"requestId":R,
   "nodes":[{"name":"red","mac":"AA:..","peers":[{"name":"gold","mac":"11:..","rssi":-61}]}]}
  ```
- Build with the project's ArduinoJson pattern (mirror an existing `*json` espnow command). Emit
  nothing via `broadcastOutput` on the JSON path. The `{` makes it auto-exempt from the stamp.
- Human path keeps the prose/table from Part A (it gets a friendly `OK:`).

**B3. Web renderer** (`WebPage_ESPNow.h` ~2290 and ~2698):
- Point both fetches at `cmd: 'espnowtoporesults json'`.
- Replace the regex scraper with `JSON.parse` + render from `data.status` (`collecting`/`none`/
  `ready`) and `data.nodes[].peers[]`. Fields to render today: `node.name`, `node.mac`, `peer.name`,
  `peer.mac`, `peer.rssi`. (Old UI also showed `Path:` / `Peers: N` / heartbeats — confirm whether
  the UI still needs them; if so they must be added to the JSON in B1/B2, but live data doesn't carry
  them.)
- This also *fixes* a current quirk: during collection the page shows an empty card; with a
  `collecting` status it can show "discovering…".

**B-verify:** build green; on web, Discover Topology renders nodes/peers from JSON with no parse
errors; serial `espnowtoporesults` = prose table (`OK:`), `espnowtoporesults json` = `{…}` blob.
**B-rollback:** its own commit on top of Part A; revert without touching the stamp foundation.
