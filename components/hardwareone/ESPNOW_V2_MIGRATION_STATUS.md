# ESP-NOW v2 Migration Status

## Overview
Migration from legacy ESP-NOW system to unified v2 transport with fragmentation, reliability, and mesh forwarding.

---

## ✅ COMPLETED (Milestone 5+)

### Direct Send Path (v2 by default)
- **Small messages (<250 bytes)**: Use `sendDirectV2Small()` with v2 envelope
  - Format: `{"v":2, "id":<msgId>, "data":"<base64>"}`
  - Optional ACK wait (runtime toggle: `espnow rel ack on`)
  - Optional dedup (runtime toggle: `espnow rel dedup on`)
  
- **Large messages (>250 bytes)**: Use `sendDirectFragmentedV2()` with JSON fragmentation
  - Format: `{"v":2, "id":<msgId>, "frag":{"i":<index>, "n":<total>}, "data":"<base64>"}`
  - Supports up to 32 fragments (V2_FRAG_MAX)
  - Automatic reassembly on RX
  - Optional ACK on completion

### Receive Path (v2 decoding)
- **v2 small envelope**: Decoded in `onEspNowDataReceived` before dispatch
- **v2 fragments**: Reassembled via `v2_frag_try_reassembly()`
- **Dedup**: Message-level dedup by (src, id) prevents duplicate deliveries
- **Legacy compatibility**: Still accepts old CHUNK/STREAM if `LEGACY_CHUNK_ENABLED=1`

### Mesh Send Path (v2 envelope)
- **`sendViaMeshEnvelope()`**: Now uses v2 format
  - Format: `{"v":2, "id":<msgId>, "src":"<mac>", "dst":"<mac>", "ttl":5, "type":"MESH", "msg":"<payload>"}`
  - TTL=5 default for flood forwarding
  - Includes version field for future compatibility

### Mesh Receive/Forward Path (v2 with TTL+dedup)
- **v2 mesh envelope detection**: Checks `"v":2` field in JSON
- **TTL-based forwarding**: Decrements TTL, forwards if >0, drops if expired
- **Dedup**: Uses v2 dedup table to prevent duplicate forwards
- **Destination check**: Delivers to self if `dst` matches or is "broadcast"
- **Unwrapping**: Extracts `msg` field and re-processes as regular message

### Runtime Toggles (CLI)
```
espnow v2log [on|off]       # Enable v2 debug logging
espnow frag rx [on|off]     # Toggle RX reassembly (default: ON)
espnow frag tx [on|off]     # Toggle TX fragmentation (default: ON)
espnow rel ack [on|off]     # Toggle ACK/wait (default: OFF)
espnow rel dedup [on|off]   # Toggle dedup (default: OFF)
```

### Compile-Time Flags
```c
ESPNOW_V2_FRAG=1           // v2 fragmentation (ON)
ESPNOW_V2_REL=1            // v2 reliability scaffold (ON)
LEGACY_CHUNK_ENABLED=0     // Legacy CHUNK/STREAM (OFF by default)
```

### Crash Fixes
- **NULL pointer protection**: All message handlers validate `ctx.recvInfo` before dereferencing
- **Dispatch fallback**: Unprefixed messages treated as plain text (backward compatible)
- **Safe error handling**: Graceful degradation when v2 features disabled

---

## 🚧 IN PROGRESS / TODO

### Mesh Fragmentation
- **Status**: Mesh currently truncates messages >200 bytes
- **Next**: Apply v2 fragmentation to mesh envelopes
  - Fragment large mesh payloads using same v2 frag logic
  - Forward fragments untouched (only destination reassembles)
  - Requires: Mesh-aware fragment forwarding

### Base64 Migration for Mesh
- **Status**: Mesh `msg` field is plain text (not base64)
- **Next**: Migrate to `data` field with base64 encoding
  - Consistent with direct v2 small envelope
  - Handles binary payloads safely
  - Format: `{"v":2, "id":..., "src":..., "dst":..., "ttl":..., "data":"<base64>"}`

### Hop-by-Hop ACK (Mesh)
- **Status**: Mesh forwarding has no ACK confirmation
- **Next**: Implement hop-by-hop ACK for mesh reliability
  - Each forwarder sends ACK back to previous hop
  - Sender can retry if no ACK received
  - Separate from end-to-end ACK (optional)

### File Transfer v2
- **Status**: File transfer still uses legacy `FILE_START/CHUNK/END` format
- **Next**: Migrate to v2 fragmentation
  - Use v2 frag for file chunks
  - Maintain file transfer state machine
  - Add progress tracking via v2 metrics

### Topology/Discovery v2
- **Status**: Topology uses legacy JSON envelope format
- **Next**: Migrate to v2 mesh envelope
  - Topology requests/responses as v2 mesh messages
  - TTL-based propagation
  - Dedup prevents duplicate topology floods

---

## 📊 METRICS & OBSERVABILITY

### Router Metrics (available via `espnow routerstats`)
```
V2 Fragments:
  - TX Fragments: <count>
  - RX Fragments: <count>
  - RX Completed: <count>
  
V2 Reliability:
  - Ack TX: <count>
  - Ack RX: <count>
  - Dedup Drops: <count>
```

### Debug Flags
```
DEBUG_ESPNOW_ROUTER    # Core routing decisions
DEBUG_ESPNOW_STREAM    # Message flow and handlers
[V2_FRAG]              # Fragment TX/RX
[V2_SMALL]             # Small envelope TX/RX
[MESH_V2]              # v2 mesh operations
[MESH_V2_RX]           # v2 mesh receive/forward
```

---

## 🎯 MIGRATION BENEFITS

### Reliability
- ✅ End-to-end ACK for critical messages
- ✅ Message-level dedup (no duplicate deliveries)
- ✅ Predictable fragmentation (up to 32 fragments vs 10)
- 🚧 Hop-by-hop ACK for mesh (TODO)

### Maintainability
- ✅ Unified envelope format (v2 marker)
- ✅ Consistent base64 encoding for data
- ✅ Single fragmentation logic (no CHUNK vs STREAM)
- ✅ Runtime toggles for testing/debugging

### Mesh-Ready
- ✅ TTL-based flood forwarding
- ✅ Dedup at forwarders
- ✅ Structured envelope for routing
- 🚧 Fragment forwarding (TODO)
- 🚧 Hop-by-hop reliability (TODO)

### Overhead Trade-off
- **Cost**: ~33% airtime overhead from base64 + JSON envelope
- **Benefit**: Fewer retransmits, easier debugging, future-proof
- **Net**: Favorable in noisy environments

---

## 🧪 TESTING

### Direct Mode Test (two devices)
```bash
# Both devices
espnow init
espnow mode direct
espnow v2log on
espnow frag rx on
espnow rel ack on
espnow rel dedup on

# Sender
espnow frag tx on
espnow bigsend <peer_name> 2000

# Receiver
espnow routerstats
```

**Expected**:
- V2 Fragments: RX Fragments > 0, RX Completed > 0
- V2 Reliability: Ack RX > 0
- Logs: `[V2_FRAG] Reassembly complete`, `[V2_SMALL]` for small messages

### Mesh Mode Test (three+ devices)
```bash
# All devices
espnow init
espnow mode mesh
espnow v2log on
espnow rel dedup on

# Master
espnow send <worker_name> "test message"

# Workers
# Check logs for [MESH_V2_RX] forwarding messages
```

**Expected**:
- `[MESH_V2_RX] Received v2 envelope`
- `[MESH_V2_RX] Forwarding: ttl=5->4`
- Dedup prevents duplicate forwards

---

## 📝 NEXT STEPS (Priority Order)

1. **Mesh Fragmentation** (HIGH)
   - Enable v2 frag for mesh envelopes >200 bytes
   - Test multi-hop fragment forwarding
   
2. **Base64 Mesh Payload** (MEDIUM)
   - Migrate mesh `msg` → `data` with base64
   - Update RX unwrapping logic
   
3. **Hop-by-Hop ACK** (MEDIUM)
   - Implement forwarder ACK back to sender
   - Add retry logic for mesh sends
   
4. **File Transfer v2** (LOW)
   - Replace FILE_* with v2 frag
   - Maintain progress tracking
   
5. **Topology v2** (LOW)
   - Migrate topology to v2 mesh envelope
   - Clean up legacy topology code

---

## 🔧 COMPATIBILITY

### Backward Compatibility
- **RX**: Accepts both v2 and legacy formats
- **TX**: Defaults to v2, can toggle to legacy via CLI
- **Migration**: Gradual - devices can run mixed v2/legacy

### Forward Compatibility
- **Version field**: `"v":2` allows future v3, v4, etc.
- **Extensible**: Can add new fields without breaking parsers
- **Toggles**: Runtime flags allow A/B testing

---

## ✅ SUMMARY

**What Works Now**:
- Direct send/receive with v2 small envelope and fragmentation
- Mesh send with v2 envelope format (TTL, version field)
- Mesh receive/forward with TTL decrement and dedup
- Runtime toggles for ACK, dedup, fragmentation
- NULL-safe message handlers (crash-proof)

**What's Next**:
- Mesh fragmentation for large payloads
- Base64 encoding for mesh payloads
- Hop-by-hop ACK for mesh reliability
- File transfer and topology migration to v2

**Migration Status**: ~80% complete
- Core v2 infrastructure: ✅ DONE
- Direct path: ✅ DONE
- Mesh path: ✅ BASIC DONE, 🚧 ADVANCED TODO
- Legacy features: 🚧 MIGRATION PENDING
