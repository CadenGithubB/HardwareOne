# ESP-NOW Reliability & Debug Logging - MANDATORY

## Changes Made

### 1. Reliability Now Mandatory ✅

**ACK and Dedup are ALWAYS enabled** - no longer optional.

```c
// Runtime defaults (MANDATORY - cannot be disabled)
static bool gV2ReliabilityEnabled = true;   // ACK enabled by default
static bool gV2DedupEnabled = true;         // Dedup enabled by default
```

**CLI Command Updated**:
```bash
espnow rel
# Output: "rel: ack=on, dedup=on (compile=on, MANDATORY - always enabled)"

espnow rel ack off
# Output: "Reliability (ACK+dedup) is MANDATORY and always enabled for robust operation."
```

**Why Mandatory**:
- Prevents duplicate message processing (dedup)
- Ensures message delivery confirmation (ACK)
- Eliminates entire class of bugs (lost messages, duplicates)
- Minimal overhead compared to reliability gains
- Production systems should never disable these

---

### 2. Extensive Debug Logging Added ✅

**Every ESP-NOW operation is now logged** with clear visual indicators.

#### Visual Indicators
- `✓` - Success
- `✗` - Failure/Error
- `→` - Forwarding
- `===` - Section headers

#### Debug Categories

**TX Callback** (`[TX_CALLBACK]`):
```
[TX_CALLBACK] === ESP-NOW SEND CALLBACK === status=SUCCESS
[TX_CALLBACK] Destination MAC: AA:BB:CC:DD:EE:FF
```

**RX Callback** (`[RX_CALLBACK]`):
```
[RX_CALLBACK] ========================================
[RX_CALLBACK] === ESP-NOW RECEIVE CALLBACK ENTRY ===
[RX_CALLBACK] ========================================
[RX_CALLBACK] Message length: 150 bytes
[RX_CALLBACK] Source MAC: AA:BB:CC:DD:EE:FF
[RX_CALLBACK] RSSI: -45 dBm
[RX_CALLBACK] Device paired: YES, encrypted: YES, name: lightblue
[RX_CALLBACK] Raw message (first 80 chars): {"v":2,"id":12345,...}
[RX_CALLBACK] Message type detection starting...
[RX_CALLBACK] Checking for v2 fragments (gV2FragRxEnabled=true)
[RX_CALLBACK] Attempting handleIncomingV2 dispatch...
[RX_CALLBACK] ✓ Message handled by v2 dispatch system
[RX_CALLBACK] ========================================
```

**Fragment Reassembly** (`[V2_FRAG_REASM]`):
```
[V2_FRAG_REASM] Fragment detected: id=12345, i=0, n=5
[V2_FRAG_REASM] Stored fragment 1/5 (id=12345, received=1/5)
[V2_FRAG_REASM] Waiting for more fragments: 1/5 received
...
[V2_FRAG_REASM] Stored fragment 5/5 (id=12345, received=5/5)
[V2_FRAG_REASM] All fragments received! Reassembling...
[V2_FRAG_RX] ✓ Reassembly complete: 512 bytes
[V2_FRAG_RX] Reassembled content (first 80 chars): ...
```

**Deduplication** (`[V2_DEDUP]`):
```
[V2_DEDUP] ✓ New message: id=12345 from AA:BB:CC:DD:EE:FF (stored in slot 5)
[V2_DEDUP] ✗ DUPLICATE DETECTED: id=12345 from AA:BB:CC:DD:EE:FF
```

**ACK Operations** (`[V2_ACK_TX]`, `[V2_ACK_RX]`):
```
[V2_ACK_TX] Sending ACK for id=12345 to AA:BB:CC:DD:EE:FF
[V2_ACK_TX] ACK frame: {"v":1,"k":"ack","id":12345}
```

**Message Dispatch** (`[DISPATCH]`):
```
[DISPATCH] ========================================
[DISPATCH] === MESSAGE DISPATCH ENTRY ===
[DISPATCH] ========================================
[DISPATCH] Message length: 150 bytes
[DISPATCH] First char: '{' (0x7B)
[DISPATCH] Content (first 80 chars): {"v":2,"id":12345,...}
[DISPATCH] Routing to JSON handler
```

**Message Handlers** (`[HANDLER]`):
```
[HANDLER] === handleJsonMessage ENTRY ===
[HANDLER] Message length: 150 bytes
[HANDLER] Content (first 80 chars): {"v":2,"id":12345,...}

[HANDLER] === handlePlainTextMessage ENTRY ===
[HANDLER] Message: test message
[HANDLER] Device: lightblue, Encrypted: YES

[HANDLER] === handleCommandMessage ENTRY ===
[HANDLER] Command: CMD:admin:pass:reboot
[HANDLER] From: lightblue, Encrypted: YES
```

**Mesh v2 Operations** (`[MESH_V2_RX]`):
```
[MESH_V2_RX] ========================================
[MESH_V2_RX] V2 MESH ENVELOPE DETECTED
[MESH_V2_RX] ========================================
[MESH_V2_RX] Message ID: 12345
[MESH_V2_RX] TTL: 5
[MESH_V2_RX] Type: MESH
[MESH_V2_RX] Source: AA:BB:CC:DD:EE:FF
[MESH_V2_RX] Destination: 11:22:33:44:55:66
[MESH_V2_RX] ✓ Message for me: 150 bytes, content: test message
[MESH_V2_RX] → Forwarding: id=12345, ttl=5->4, dst=11:22:33:44:55:66
[MESH_V2_RX] ✗ TTL expired, dropping id=12345
```

**Fragment TX** (`[V2_FRAG_TX]`):
```
[V2_FRAG_TX] ✓ Fragment 1/5 sent (id=12345, len=156)
[V2_FRAG_TX] ✓ Fragment 2/5 sent (id=12345, len=156)
...
[V2_FRAG_TX] ✓ All 5 fragments sent for id=12345 (total 512 bytes)
```

**Small Envelope TX** (`[V2_SMALL_TX]`):
```
[V2_SMALL_TX] Sending: id=12345, len=87 bytes
[V2_SMALL_TX] ✓ Send successful (no ACK wait)
[V2_SMALL_TX] ✓ ACK received for id=12345
[V2_SMALL_TX] ✗ ACK timeout for id=12345
```

---

### 3. Error Detection

**NULL Pointer Checks**:
```
[RX_CALLBACK] CRITICAL ERROR: recv_info is NULL!
[RX_CALLBACK] CRITICAL ERROR: incomingData is NULL!
[HANDLER] CRITICAL ERROR: handlePlainTextMessage called with NULL recvInfo
```

**Invalid Data**:
```
[V2_FRAG_REASM] Invalid fragment indices: i=5, n=5
[V2_FRAG_REASM] ERROR: No reassembly slot available (max=4)
[V2_FRAG] n=50 exceeds max=32, dropping
```

**Warnings**:
```
[RX_CALLBACK] WARNING: rx_ctrl is NULL (no RSSI)
[TX_CALLBACK] WARNING: NULL MAC address in callback
```

---

## How to Use

### Enable Debug Output

```bash
# Enable ESP-NOW router debug flag
debug espnow_router on

# Or enable all ESP-NOW debug
debug espnow_stream on
debug espnow_router on
```

### Monitor Serial Output

When a message is received, you'll see the complete flow:
```
[RX_CALLBACK] === ESP-NOW RECEIVE CALLBACK ENTRY ===
[RX_CALLBACK] Message length: 150 bytes
[RX_CALLBACK] Source MAC: E8:6B:EA:2F:F3:68
[RX_CALLBACK] RSSI: -45 dBm
[RX_CALLBACK] Device paired: YES, encrypted: YES, name: lightblue
[RX_CALLBACK] Raw message: {"v":2,"id":12345,...}
[V2_DEDUP] ✓ New message: id=12345 from E8:6B:EA:2F:F3:68
[DISPATCH] === MESSAGE DISPATCH ENTRY ===
[DISPATCH] Routing to JSON handler
[HANDLER] === handleJsonMessage ENTRY ===
[V2_ACK_TX] Sending ACK for id=12345 to E8:6B:EA:2F:F3:68
[RX_CALLBACK] ✓ Message handled by v2 dispatch system
```

### Troubleshooting

**Message Not Received**:
- Check for `[RX_CALLBACK] === ESP-NOW RECEIVE CALLBACK ENTRY ===`
- If missing: ESP-NOW not receiving at all (pairing issue, encryption mismatch)
- If present: Check subsequent logs for where processing fails

**Duplicate Messages**:
- Look for `[V2_DEDUP] ✗ DUPLICATE DETECTED`
- This is NORMAL and expected - dedup is working correctly
- Duplicates are automatically dropped

**Fragment Issues**:
- Check `[V2_FRAG_REASM]` logs
- Look for "Waiting for more fragments: X/Y received"
- If stuck: Fragment lost in transit, sender should retry

**ACK Timeouts**:
- Look for `[V2_SMALL_TX] ✗ ACK timeout`
- Indicates receiver didn't send ACK or ACK was lost
- Message will be retried automatically

**Mesh Forwarding**:
- Check `[MESH_V2_RX]` logs
- Look for `→ Forwarding` or `✓ Message for me`
- TTL decrements show hop count

---

## Performance Impact

**Debug Logging Overhead**:
- Minimal CPU impact (only when DEBUG_ESPNOW_ROUTER enabled)
- No impact when debug disabled
- Logs are asynchronous (non-blocking)

**Reliability Overhead**:
- ACK: +1 small message per transmission (~50 bytes)
- Dedup: Minimal memory (32 entries × 16 bytes = 512 bytes)
- Net benefit: Fewer retransmits, higher reliability

---

## Summary

**Reliability is now MANDATORY**:
- ✅ ACK enabled by default
- ✅ Dedup enabled by default
- ✅ Cannot be disabled via CLI
- ✅ Production-ready configuration

**Debug logging is COMPREHENSIVE**:
- ✅ Every RX/TX operation logged
- ✅ Fragment reassembly tracked
- ✅ Dedup decisions visible
- ✅ ACK operations traced
- ✅ Mesh forwarding detailed
- ✅ Error conditions highlighted
- ✅ Visual indicators for quick scanning

**Result**: You can now see EXACTLY where any ESP-NOW issue occurs.
