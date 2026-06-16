# ESP-NOW retrievable payloads — chunk-store-stitch (design note)

**Status:** design only, not yet implemented. Captures the direction agreed 2026-06-16.
**Relates to:** the `reqId` correlation work (already in `System_ESPNow.cpp`) and
`docs/ESPNOW_JSON_CONTRACT.md`.

## The problem

"Middle-tier" payloads — things bigger than one ESP-NOW frame that a consumer
reads back *later* — are handled badly today:

- A remote **command result** is reassembled into a transient 6144-byte buffer
  (`deferredCmdRespResult`), then copied into a **256-byte history slot**, where
  it is **truncated to 255 chars**. So `espnowmessages json` returns clipped
  command output even though the full result existed moments earlier.
- **Long text** (`espnowsend`) is rejected outright at 202 bytes — it never even
  enters the pipeline (single-frame-only guard in `cmd_espnow_send`, mirrored by
  a receive guard in `v4h_text`).

Both stem from the same root: the constrained device tries to **materialize a
large logical object in its scarce RAM**, then can't afford to keep it.

## The three tiers

| Tier | Examples | Handling |
|------|----------|----------|
| **1. Tiny / single-frame** | heartbeats, short text, sensor readings, metadata | store as-is (no change) |
| **2. Medium / retrievable blobs** | **command results**, long text, topology results, capability/schema dumps | **chunk-store-stitch (this note)** |
| **3. File-sized** | actual files | stream to disk via the file pipeline (already solved) |

Tier 2 is the gap. It is currently a mess of per-type reassemble-then-truncate
buffers; this note replaces all of it with one mechanism.

## The mechanism: device stores fragments, the edge assembles

Principle: **a memory-tight mesh node should be a dumb store of small tagged
records; assembly and interpretation belong at the resource-rich edge** (browser,
phone/BLE, G2, or another node), where RAM is abundant.

- **Sender** splits a large payload into ≤200-byte pieces, each tagged
  `{groupId, piece#, total}` (for command results `groupId == reqId`; for text a
  per-message id). Each piece is a normal single-frame message.
- **Device storage** keeps each piece in an ordinary history slot, carrying its
  tags. No reassembly buffer, no big lump, no truncation. RAM stays flat.
- **Consumer** reads `espnowmessages json`, groups by `groupId`, orders by
  `piece#`, concatenates → the full payload, assembled in its own RAM.

The transport already stamps every fragment with `msgId / fragIndex / fragCount`
(it uses them to reassemble). This design **stores those tags instead of
consuming them** — minimal new data.

## Composition with `reqId`

The `reqId` correlation already added is exactly the group key for command
results:

- `reqId` answers **which** request a result belongs to.
- chunk-store-stitch delivers the **full content** instead of 255 clipped bytes.

Together they complete the Phase-3 goal ("app gets the complete result of a
relayed remote command").

## Rollout order

1. **Command results first** — biggest payoff, group key (`reqId`) already
   exists, and it fixes a live truncation bug.
2. **Long text** falls out of the same mechanism (per-message group id).
3. **Topology / schema dumps** — optional later.

## Explicitly out of scope

- **Files** → already go to disk (unlimited, durable). Do not reinvent.
- **Sensor streams / telemetry** → ephemeral, throughput-oriented, correlated by
  `seqNum` for ordering/loss, not assembled into a stored blob.
- **Heartbeats / control frames** → tiny, single-frame, nothing to assemble.

## Known costs

- Each consumer (web, BLE, G2) needs a small group-and-stitch helper; a consumer
  that doesn't implement it sees loose pieces (degraded, not broken).
- **Partial messages**: the history ring is finite (100 slots/peer). If older
  pieces are evicted while newer ones remain, the consumer sees a gap and must
  render "partial" gracefully. A very large message can crowd the ring.
- Per-piece delivery: a large payload becomes N small messages = N ACKs; the
  "delivered" UI flips when all pieces land.
- It is a new on-wire convention ("chunked record") that every reader must agree
  on.
