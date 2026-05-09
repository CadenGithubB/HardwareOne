#ifndef RING_BRIDGE_SEQUENCE_H
#define RING_BRIDGE_SEQUENCE_H

// =============================================================================
// Ring → Glasses → ESP Bridge Sequence — Implementation Reference
// =============================================================================
//
// Goal: get sensor data (HR, SpO2, HRV, temp, steps, battery, etc.) from the
// R1 ring delivered to the ESP firmware via the G2 glasses, instead of
// connecting to the ring directly. This avoids the pkey landmine flagged
// in components/hardwareone/G2_Ring.h:18-21.
//
// **Source provenance**: the entire sequence below is derived from
// docs/FlutterApp-main/, a community RE project. The base-protobuf field
// numbers and SID values are likely accurate; the exact step ordering and
// timing constants are taken verbatim from FlutterApp's production code
// path (not its tests). See per-step file:line citations.
//
// **Big simplification opportunity**: FlutterApp's R1Manager has a comment
// at lib/src/services/r1_manager.dart:258-261 saying:
//
//   "As far as I can tell, this doesn't actually matter - as long as you
//    send the right RING_CONNECT_INFO to the glasses, it seems to connect
//    anyway. I'm not sure if it's not actually making use of this mac
//    address or there is something else going on, but just to match
//    original app behavior, I'm keeping this here"
//
// If true, the firmware can implement the bridge WITHOUT ever connecting
// to the ring directly — just connect to glasses, AUTH/PIPE_ROLE/TIME_SYNC,
// then send RING_CONNECT_INFO with the ring's MAC, and the glasses do the
// rest. **Test this on your hardware before committing to a more complex
// path.** It would also avoid the pkey question entirely.
//
// =============================================================================
// Wire-protocol primer (already in your firmware):
//
// Glasses use the G2 envelope (System_G2_Protocol.h) on UUIDs in BLE_IDF.
// All bridge messages on the GLASSES side are sent on:
//     sid=128 (UX_DEVICE_SETTINGS_APP_ID)
// with the DevCfgDataPackage protobuf wrapper. See dev_config_protocol.proto.
//
// Ring uses the R1 envelope (R1_RE_Reference.h SECTION 2). All ring-side
// messages here use module=system (0x01), cmd=system (0x00), and vary
// only by subCmd byte and payload.
//
// =============================================================================
// PHASE 1 — GLASSES STARTUP (must run after BLE link is up)
// =============================================================================
// Source: lib/src/services/g2_manager.dart:316-346 (_runStandardSetup)
// Source: lib/src/protocol/g2_messages.dart:33-97 (builders)
//
// Sent to BOTH arms (right and left) at first; only the right arm gets
// the role-change. NB: firmware currently does NOT send any of these
// (sid=0x80 is in the brick blocklist). If the bridge requires AUTH to
// be honoured by the glasses, the brick block must be lifted FIRST —
// see the warning in dev_config_protocol.proto.
//
// Note: existing firmware connection works without these on the visible
// rendering subsystems (sid=0xE0 EvenCore). The bridge specifically may
// require them because the ring-relay path uses sid=128's machinery.

// --- Step 1.1 (right arm): AUTHENTICATION (cmd=4) ---------------------------
// Wrapper:  DevCfgDataPackage { commandId=4, magicRandom=<u8>, authMgr={
//             secAuth=true, phoneType=PHONE_ANDROID(=4)
//           } }
// SID:      128 (G2RE_SID_UX_DEVICE_SETTINGS_APP_ID)
// Wait:     no explicit ack; standard BLE write completes
// Timing:   on iOS, FlutterApp inserts a 500 ms delay before this; not
//           required on Android.
//
// --- Step 1.2 (right arm only): PIPE_ROLE_CHANGE (cmd=5) --------------------
// Wrapper:  DevCfgDataPackage { commandId=5, magicRandom=<u8>, roleChange={
//             asCmdRole=RIGHT(=1)
//           } }
// SID:      128
// Purpose:  declares this arm as the "command" (right) side; left arm
//           does NOT receive this.
//
// --- Step 1.3 (right arm): TIME_SYNC (cmd=128) ------------------------------
// Wrapper:  DevCfgDataPackage { commandId=128, magicRandom=<u8>, timeSync={
//             timestamp=<unix seconds, s32>,
//             timezone =<int64 of (utc_offset_minutes / 15)>
//           } }
// SID:      128
// **TIMEZONE ENCODING WARNING**: G2 uses QUARTER-HOURS
// (FlutterApp: tzQuarterHours = now.timeZoneOffset.inMinutes ~/ 15).
// R1 (Phase 2 below) uses RAW MINUTES for the same concept. Easy to
// confuse; keep them straight.
//
// LEFT arm setup, if you also connect to it:
//   1.1 AUTH (same as right)
//   1.3 TIME_SYNC (same as right)
//   (NO PIPE_ROLE_CHANGE on left)
//
// --- Step 1.4: Heartbeat (cmd=14, every 30 s) ------------------------------
// Wrapper:  DevCfgDataPackage { commandId=14, magicRandom=<u8>, baseHeartBeat={} }
// SID:      128
// Cadence:  30 s. FlutterApp does this on each connected arm independently.
// Note:     This is in ADDITION to firmware's existing EvenCore heartbeat
//           (sid=0xE0 cmd=12). Both may need to be alive for the bridge.

// =============================================================================
// PHASE 2 — RING STARTUP (only required if direct ring connection used)
// =============================================================================
// Source: lib/src/services/r1_manager.dart:156-177 (buildStandardSetupSequence)
// Source: lib/src/protocol/r1_messages.dart:183-264
//
// **SKIP THIS PHASE IF testing the "RING_CONNECT_INFO alone" simplification**
// (see top of file). In that case, after Phase 1 + 3, the glasses handle
// the ring directly — no R1 BLE central role on the ESP needed.
//
// If you DO connect to the ring directly, the production sequence is
// 3 steps (NOT 7 — the longer list seen in tests is per-feature subscribe
// extras). MTU negotiation (target 247) happens before this.

// --- Step 2.1: ring.pairAuth (subCmd=0x08, payload=[0x01]) -----------------
// **PKEY WARNING**: firmware notes from a separate RE thread say pairAuth
// requires a server-issued pkey. FlutterApp sends just `[0x01]` and gets
// OK in test fixtures. If your rings reject this with status=ack/refuse
// or status=ack/notSupport, the pkey hypothesis is real and you must
// fall back to the "skip Phase 2" approach.
// Wait:  ack with serialId match (4 s timeout in FlutterApp), then 1 s
//        delay before next step.

// --- Step 2.2: ring.systemTime (subCmd=0x05) -------------------------------
// Payload (8 bytes): [tz_minutes_LE_i16] [unix_seconds_LE_u32]
// **TIMEZONE ENCODING WARNING**: R1 uses RAW MINUTES (NOT quarter-hours
// like G2 does in Phase 1.3). FlutterApp:
//   tzMinutes = now.timeZoneOffset.inMinutes
//   payload = i16(tzMinutes) ++ u32(unixSeconds)
// Status method: 'set' (vs 'get' for the other steps).
// Wait:  ack, then 200 ms delay.

// --- Step 2.3: ring.advStart (subCmd=0x0A) ---------------------------------
// Payload: 6-byte G2 right-arm BLE MAC, IN ORDER (NOT reversed).
// (Compare to Phase 3 below where the same MAC IS reversed when sent
// to the glasses. Inconsistency confirmed in source.)
// Wait:  ack, then 200 ms delay.
// Per the comment cited at top: "this doesn't actually matter".

// =============================================================================
// PHASE 3 — BRIDGE TRIGGER (this is THE message that establishes the bridge)
// =============================================================================
// Source: lib/src/services/ring_bridge_coordinator.dart
// Source: lib/src/protocol/g2_messages.dart:99-119 (buildConnectRing)

// --- Step 3.1: glasses.RING_CONNECT_INFO (cmd=6) ---------------------------
// SID:      128 (G2RE_SID_UX_DEVICE_SETTINGS_APP_ID)
// Wrapper:  DevCfgDataPackage { commandId=6, magicRandom=<u8>, ringInfo={
//             connectRing=true,
//             ringMac=<6 bytes, REVERSED from BLE address order>,
//             ringName=<UTF-8 bytes of name, NOT null-terminated>
//           } }
//
// **MAC BYTE-REVERSAL IS REQUIRED HERE**:
//   FlutterApp: ringMac.reversed.toList(growable: false)
//   So if BLE address is "AA:BB:CC:11:22:33", the bytes sent are:
//     [0x33, 0x22, 0x11, 0xCC, 0xBB, 0xAA]
//
// Sent to: right arm (the "command" arm, established by Phase 1.2).
//
// After this single message, the glasses initiate their own BLE
// connection to the ring (using the MAC and name provided), and
// will start forwarding ring data to the host on sids 144 + 145
// (Phase 4).

// --- Step 3.2 (optional, FlutterApp-style): RingDataPackage(BLE_ADV) ------
// Source: lib/src/protocol/g2_messages.dart:121-139 (buildRingBleAdv)
//
// This builder exists in FlutterApp but does NOT appear to be called from
// ring_bridge_coordinator.dart in the version we examined. It would send:
//   SID:      145 (UX_RING_DATA_RELAY_ID)
//   Wrapper:  RingDataPackage { commandId=EVENT(1), magicRandom=<i32>,
//                               event={ ringMac=<6 bytes BLE order?>,
//                                       eventId=BLE_ADV(1) } }
//
// Treat as optional / try only if RING_CONNECT_INFO alone doesn't trigger
// the bridge.

// =============================================================================
// PHASE 4 — RECEIVING DATA (the payoff)
// =============================================================================
// Source: lib/src/protocol/g2_proto_decoder.dart:59-60
// Source: docs/g2_proto/ring.proto
//
// After Phase 3 succeeds, the glasses begin pushing ring telemetry on:
//
//   sid=144 (UX_RING_ROW_DATA_ID)   — RingDataPackage with rawData populated
//                                     (battery, hr, spo2, hrv, temp, kcal,
//                                      steps + their timestamps)
//   sid=145 (UX_RING_DATA_RELAY_ID) — RingDataPackage with event populated
//                                     (currently only BLE_ADV)
//
// In addition, ring touch gestures (if any) appear on sid=224 (EvenHub /
// EvenCore — the firmware's existing rendering surface) as standard touch
// events with EventSourceType=TOUCH_EVENT_FROM_RING(=2). See
// G2_RE_Reference.h SECTION 5.
//
// **Important**: ring data does NOT arrive in raw R1 envelope format on
// the bridge SIDs. The glasses' firmware unpacks the R1 envelope, extracts
// the values, and re-encodes them as a G2 protobuf. This means firmware
// only needs to:
//   1. Recognise sid=144 and sid=145 in handleEnvelope() routing
//   2. Decode the payload as a RingDataPackage protobuf (hand-rolled
//      g2PbRead* primitives are sufficient — only 4 fields at the wrapper
//      level, then 17 fields in RingRawData or 4 fields in RingEvent)
//
// Suggested integration points in firmware:
//   - System_G2_Protocol.h: add G2_SID_RING_RAW (0x90) and G2_SID_RING_RELAY
//     (0x91) constants alongside the existing G2_SID_* set
//   - G2_Glasses.cpp: extend handleEnvelope() to dispatch these to a new
//     parseRingDataPackage() function
//   - Surface fields to whatever consumer wants them (a sensors page,
//     MQTT publisher, web UI panel, SD log, etc.)

// =============================================================================
// PHASE 5 — KEEPALIVE
// =============================================================================
// Source: lib/src/services/g2_manager.dart:357-375
// Source: lib/src/services/r1_manager.dart:225-241
//
// FlutterApp keeps three heartbeats running once the bridge is up:
//   - G2 right arm: BASE_CONNECT_HEART_BEAT (sid=128 cmd=14) every 30 s
//   - G2 left arm:  same, every 30 s (if connected)
//   - R1 ring:      heartbeatPack (subCmd=0x7F) every 30 s, no ack required
//
// Your firmware also has its own EvenCore heartbeat (sid=0xE0 cmd=12).
// Empirically determine whether ALL of these are required or whether your
// existing one alone is sufficient. The glasses may time out the bridge
// path independently of the rendering path.

// =============================================================================
// FAILURE MODES (from FlutterApp source)
// =============================================================================
// - Ring pairAuth fail:    ring sends error frame (status bits show ack=error
//                          or ack=notSupport). FlutterApp logs and aborts setup.
// - advStart timeout:      4 s ack window in FlutterApp; on miss, tear down
//                          and reconnect.
// - RING_CONNECT_INFO fail: glasses can refuse if MAC doesn't match the
//                          ring's actual advertised MAC; no explicit timeout
//                          in FlutterApp's G2Manager — manual retry only.
// - Bridge silently dead:  no "bridge up" event from device; FlutterApp
//                          assumes success after step 3.1 + 3.2 complete.
//                          Real signal is sid=144/145 traffic appearing.

// =============================================================================
// QUICK START FOR FIRMWARE TEST (lowest-risk validation order)
// =============================================================================
//
// 1. With the brick block IN PLACE (don't lift it yet), capture sid=0x80
//    RX traffic for 60 s while the glasses are idle. If the only bytes
//    you see are `08 06 10 ?? 2A 00`-shaped (cmd=6 RING_CONNECT_INFO with
//    empty body), that confirms the schema and your brick fears stem from
//    sending malformed payloads, not from sid=0x80 itself.
//
// 2. On a unit you can recover (factory reset via case, or hardware reset
//    pin), lift the brick block for sid=0x80 ONLY for a single test,
//    send a well-formed BASE_CONNECT_HEART_BEAT (Phase 1.4 — empty body,
//    safest possible payload), and verify the device acks (status byte
//    on the response should not have an error bit set).
//
// 3. If 2 succeeds, send AUTH (Phase 1.1). Then PIPE_ROLE_CHANGE (1.2)
//    and TIME_SYNC (1.3) on the right arm.
//
// 4. Skip Phase 2 (don't connect to ring directly). Send only Phase 3.1
//    with the ring's MAC reversed and a placeholder name. Watch for
//    sid=144/145 traffic — if it appears, the simplified bridge works
//    and you're done.
//
// 5. If sid=144/145 stays silent, fall back to Phase 2 (ring direct
//    connection + advStart) and see if it activates the bridge then.
//    Validates whether the ring's advStart matters or RING_CONNECT_INFO
//    is sufficient on its own.
//
// All of this is best validated by recording with btsnoop (Android side
// when comparing with FlutterApp) or a sniffer + your existing protocol
// log labels (CMP_RX/CMP_TX/TXHDR per FlutterApp README.md:206+).

#endif // RING_BRIDGE_SEQUENCE_H
