# ESP-NOW Dead-Code Audit

**Date:** 2026-07-21  
**Type:** Findings list — candidates for removal. **Nothing has been deleted.**  
**Scope:** the ESP-NOW subsystem (`System_ESPNow*`, `OLED_ESPNow*`, `G2_Page_ESPNow*`, `WebPage_ESPNow*`), usage checked across all of `components/hardwareone/` + `main/`.

## How this was produced

A map/find/verify/synthesize sweep (130 agents). First the indirect-usage surfaces were mapped so "no direct caller" wouldn't produce false positives: the opcode handler table `kV4HandlerTable[]`, the CLI command table `espNowCommands[]`, `espnowSettingsModule`, web routes, OLED/G2 UI registries, function-pointer/deferred sites, X-macro-generated names, and `#if ENABLE_BONDED_MODE` config gating. Then finders enumerated candidates across every file, and **each candidate was independently double-verified** by two adversarial "prove it's used" passes (one hunting direct calls + tables, one hunting consumer files, macro expansion, board gating, and string/JSON dispatch).

**A symbol is listed below only if BOTH verifiers agreed it is unreferenced in every supported build config.** Still, re-confirm before deleting — the checklist at the end is built for exactly that.

**Counts:** DEAD (both verifiers agree) = **53**, board/config-gated = 0, plausible/split = 0, uncertain = 0, flagged-then-cleared-LIVE = 2.

---

## Findings by group

### Dead TX senders/helpers with no caller (System_ESPNow.cpp)

Non-static (or file-static) transmit-side functions that are defined + forward-declared but invoked nowhere. Each has a live sibling doing the real work, so these are superseded leftovers, not stubs waiting to be wired. v4_send_topo_request/v4_broadcast_topo_request form a self-contained dead island: the static helper's only caller is the dead broadcast wrapper, and the live topology initiator (requestTopologyDiscovery @7739) calls v4_send_frame directly instead.

- sendChunkedResponse @ components/hardwareone/System_ESPNow.cpp:907 — legacy v2 JSON-response helper wrapping v4_send_command_response(); proto at System_ESPNow.h:1154; zero callers
- v4_send_text @ components/hardwareone/System_ESPNow.cpp:2540 — thin wrapper over v4_send_payload_smart; fwd-decl :293; live TEXT senders use v4_broadcast/meshBroadcastEnvelopeTyped/etc. instead
- v4_broadcast_topo_request @ components/hardwareone/System_ESPNow.cpp:2284 — topo-request initiator; fwd-decl :291; device answers topo requests but never initiates a broadcast one
- v4_send_topo_request @ components/hardwareone/System_ESPNow.cpp:2269 — static; only caller is the dead v4_broadcast_topo_request:2287, so reachable only through uncalled code

### Vestigial metadata last-sent tracking globals (dead cluster)

A 4-member family of file-scope static Strings that were meant to back a 'compare-to-last-sent' dirty-diff scheme. All four are definition-only — never read, never written. Only the companion bool gMetadataChanged (:357) is live (written @10670, 10684), proving the value-snapshot dedup was never implemented. Delete the whole family together.

- gLastSentFriendlyName @ components/hardwareone/System_ESPNow.cpp:358 — def only
- gLastSentRoom @ components/hardwareone/System_ESPNow.cpp:359 — def only
- gLastSentZone @ components/hardwareone/System_ESPNow.cpp:360 — def only
- gLastSentTags @ components/hardwareone/System_ESPNow.cpp:361 — def only

### Orphaned public functions/accessors (no caller; live sibling exists)

Public functions (definition + header prototype) with zero call sites. In each case a sibling API is the one actually used, so these are redundant accessors rather than pending features. sendStatusGet's own header comment (Sessions.h:295) even states 'Has NO callers'.

- espnowCollapsedPeerMessages @ components/hardwareone/System_ESPNow.cpp:15491 — proto System_ESPNow.h:1206; per-peer collapse variant orphaned; siblings espnowGetConversation/espnowCollapsedAllMessages are the ones called by OLED/G2
- espnowtx::getStats @ components/hardwareone/System_ESPNow_Tx.cpp:230 — proto Tx.h:208; txTask reads sStats directly under spinlock (:124-127); header 'CLI can read via getStats()' claim is false
- sendStatusGet @ components/hardwareone/System_ESPNow_Sessions.cpp:738 — proto Sessions.h:297; web UI uses sendStatusSlotCount/sendStatusAt snapshot walk; header comment admits no callers
- oledEspNowGetMainMenuItemCount @ components/hardwareone/OLED_ESPNow.cpp:654 — proto OLED_ESPNow.h:150; callers use ESPNOW_MENU_ITEM_COUNT directly
- oledEspNowFormatMac @ components/hardwareone/OLED_ESPNow.cpp:1579 — proto OLED_ESPNow.h:162; a {return macToDisplayStr(mac);} wrapper nobody calls
- oledEspNowValidateDevicePtr @ components/hardwareone/OLED_ESPNow.cpp:1635 — proto OLED_ESPNow.h:185; sibling oledEspNowValidateMessagePtr IS used (:1505), this one isn't

### Unwired remote-file-browse feature (whole-feature dead cluster)

An entire OLED remote-file-browse sub-feature that was never wired up. There is no ESPNOW_VIEW_REMOTE_FILES enum value, so the display/input dispatchers can never route to the renderer or handler; the request stub broadcasts 'not yet implemented'. All four functions have only def+prototype. The vestigial MSG_TYPE_FILE_BROWSE macro (listed under the V2/V3 macro group) was this feature's intended wire type. Delete the cluster as a unit.

- oledEspNowSendBrowseRequest @ components/hardwareone/OLED_ESPNow.cpp:2337 — proto :188; body emits 'not yet implemented'
- oledEspNowDisplayRemoteFiles @ components/hardwareone/OLED_ESPNow.cpp:2352 — proto :189; no view enum routes to it
- oledEspNowHandleRemoteFilesInput @ components/hardwareone/OLED_ESPNow.cpp:2409 — proto :190; oledEspNowHandleInput never calls it
- storeRemoteFileBrowseResult @ components/hardwareone/OLED_ESPNow.cpp:2438 — proto :197; declared 'callable from ESP-NOW handler' but System_ESPNow.cpp never calls it

### Vestigial V2/V3 JSON envelope message-type string macros (MSG_TYPE_*)

Legacy string message-type constants from the V2/V3 JSON-envelope transport, superseded by the V4 binary opcode enum. Object-like #defines: a use site would name the token verbatim, and none do. Only sibling MSG_TYPE_BOOT is still live (v2_init_envelope @System_ESPNow.cpp:7729). Where a modern feature shares the name (TEXT, STREAM, CMD, USER_SYNC), it runs through the distinct V4 opcode path, not these strings.

- MSG_TYPE_HB @ components/hardwareone/System_ESPNow.h:53
- MSG_TYPE_ACK @ components/hardwareone/System_ESPNow.h:54 — V4 ACK is a separate numeric opcode
- MSG_TYPE_MESH_SYS @ components/hardwareone/System_ESPNow.h:55
- MSG_TYPE_RESPONSE @ components/hardwareone/System_ESPNow.h:56
- MSG_TYPE_STREAM @ components/hardwareone/System_ESPNow.h:57 — live STREAM is opcode 90/v4h_stream
- MSG_TYPE_FILE_STR @ components/hardwareone/System_ESPNow.h:59
- MSG_TYPE_CMD @ components/hardwareone/System_ESPNow.h:60 — live CMD is opcode 50
- MSG_TYPE_TEXT @ components/hardwareone/System_ESPNow.h:61 — live TEXT is opcode 52/v4h_text
- MSG_TYPE_USER_SYNC @ components/hardwareone/System_ESPNow.h:62 — live path uses ESPNOW_V4_TYPE_USER_SYNC (56)
- MSG_TYPE_FILE_BROWSE @ components/hardwareone/System_ESPNow.h:63 — belonged to the unwired remote-file-browse feature above

### Vestigial V2/V3 payload-type string macros (PAYLOAD_*)

The entire 'Payload types' string-macro block (System_ESPNow.h:66-71) is definition-only — a V2/V3-era JSON payload-type scheme fully replaced by numeric V4 opcodes. Topology/time-sync features are live but via the opcode enum (TOPO_REQ=32, TIME_SYNC=35), which does not reference these strings. Delete the block wholesale.

- PAYLOAD_CMD @ components/hardwareone/System_ESPNow.h:66
- PAYLOAD_TOPO_REQ @ components/hardwareone/System_ESPNow.h:67
- PAYLOAD_TOPO_RESP @ components/hardwareone/System_ESPNow.h:68
- PAYLOAD_QUERY @ components/hardwareone/System_ESPNow.h:69
- PAYLOAD_STATUS @ components/hardwareone/System_ESPNow.h:70
- PAYLOAD_TIME_SYNC @ components/hardwareone/System_ESPNow.h:71

### Unused mesh tuning constants (macros)

Integer #defines sitting alongside live mesh retry/dedup constants but never consulted. Their live neighbors (MESH_RETRY_QUEUE_SIZE, MESH_DEDUP_SIZE) are used; these are not. MESH_MAX_RETRIES additionally has a stale comment pointing to a non-existent espnow_system.h; actual retry paths use local const MAX_RETRIES literals instead.

- MESH_ACK_TIMEOUT_MS @ components/hardwareone/System_ESPNow.h:253 — retry queue never does a timeout comparison
- MESH_MAX_RETRIES @ components/hardwareone/System_ESPNow.h:254 — only other mention is a stale comment (System_ESPNow.cpp:371); loops use local const MAX_RETRIES=3
- MESH_DEDUP_WINDOW @ components/hardwareone/System_ESPNow.h:269 — sibling MESH_DEDUP_SIZE sizes gMeshSeen[], but the window value is unused

### Unused V4 wire flag bits

Flag enumerators in System_ESPNow_Wire.h that are never OR'd into a TX flags field nor masked on RX. ENCRYPTED is a self-documented DEPRECATED/VESTIGIAL LMK-era bit (AEAD confidentiality now signalled by ESPNOW_V4_FLAG_SESSION_FRAME + V4RxCtx::isSessionEncrypted); COMPRESS and PRIORITY_HIGH are 'future' earmarks never implemented. None are behind #if gates.

- ESPNOW_V4_FLAG_ENCRYPTED @ components/hardwareone/System_ESPNow_Wire.h:177 — 5 non-def references are all dead explanatory comments
- ESPNOW_V4_FLAG_COMPRESS @ components/hardwareone/System_ESPNow_Wire.h:182 — 'future', zero refs
- ESPNOW_V4_FLAG_PRIORITY_HIGH @ components/hardwareone/System_ESPNow_Wire.h:189 — 'Phase 5' earmark, zero refs

### Unused V4 wire size/length constants (macros)

Derived convenience constants never consumed by any static_assert, buffer-sizing, or comparison. The real values are computed inline: headerLen from sizeof(EspNowV4Header), MAX_PAYLOAD from the (250-32) literal. Their input macros are used directly; these wrappers are not.

- ESPNOW_V4_HEADER_LEN @ components/hardwareone/System_ESPNow_Wire.h:29 — only other mention is a comment on the headerLen field (:205)
- ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT @ components/hardwareone/System_ESPNow_Wire.h:47 — 186-byte cap referenced nowhere

### Dead inline helpers in headers

Free inline functions defined in headers with zero call sites in any configuration. shouldStreamSensorToRemote's own comment claims it 'replaces the 7-line block copy-pasted into every sensor task loop' — but the migration never happened, so the copy-pasted blocks remain and the helper is orphaned. bondRoleStr is a formatter defined above the ENABLE_BONDED_MODE guard (so compiled unconditionally) whose heavily-used siblings isBondMaster/isBondWorker never pull it in.

- bondRoleStr @ components/hardwareone/System_ESPNow.h:80 — inline role formatter, no callers
- shouldStreamSensorToRemote @ components/hardwareone/System_ESPNow.h:1088 — intended dedup helper never adopted by any sensor loop

### Uninstantiated structs / types

Struct types never instantiated, referenced only within their own header (or not at all). MeshTopoPeer exists solely as the element type of MeshTopoNode::peers, whose only instance gMeshTopology (System_ESPNow.cpp:329) is defined but never populated or read — the live topo RX path (v4h_topo_peer) decodes the distinct Wire.h V4PayloadTopoPeer into a String accumulator instead. RemoteSensorStatus is bypassed entirely: status flows as JSON via broadcastSensorStatus and is consumed by the scalar-param updateRemoteSensorStatus(). All member fields are dead by extension.

- MeshTopoPeer @ components/hardwareone/System_ESPNow.h:156 — dead graph-building leftover; container gMeshTopology never touched in either build config
- RemoteSensorStatus @ components/hardwareone/System_ESPNow_Sensors.h:32 — never instantiated; substring hits are the unrelated updateRemoteSensorStatus function

### Write-only field and unreached enum value

A struct field assigned but never read, and an enum state never assigned. rekeyTxSeqAtInit is written in 4 places (mark/apply/abort) but no code ever reads or compares it — a snapshot nobody consumes. FILE_SLOT_FAILED is never assigned (state only ever becomes FREE/RECEIVING/COMPLETING; failure uses the separate streamFailed bool); the diag table kStates[3]="FAILED" (System_ESPNow.cpp:11686) indexes by literal position, not the enum name, and that branch is unreachable.

- rekeyTxSeqAtInit @ components/hardwareone/System_ESPNow_Sessions.h:86 — write-only (Sessions.cpp:788,807,826; Handlers_Crypto.cpp:1230); no reader
- FILE_SLOT_FAILED @ components/hardwareone/System_ESPNow_Files.h:60 — state value never set or compared; positional-only mirror in kStates[] is unreachable

### Reserved event-category enum bits — dead as identifiers, KEEP for wire stability

Named EspNowEventCategory enumerators (System_ESPNow_Identity.h) that are never referenced by name: peerIdentityWantsEvent only ever gates on ESPNOW_EVT_HEARTBEAT and ESPNOW_EVT_SENSOR, and the wire carries raw numeric masks (parsed from user hex in espnowrequestevents; documented as hex literals like TOPO=0x04 in help text). Deleting the C identifiers changes no behavior — but ESPNOW_EVT_WORKER_STATUS's own comment ('bit kept for bitmap stability') signals these bit POSITIONS are deliberately reserved to prevent reuse in future protocol versions. Recommendation: safe to remove the names, but if kept, do NOT renumber remaining enumerators.

- ESPNOW_EVT_TOPOLOGY @ components/hardwareone/System_ESPNow_Identity.h:121 — bit 0x04, reserved wire vocab
- ESPNOW_EVT_BOND_HEARTBEAT @ components/hardwareone/System_ESPNow_Identity.h:122 — bit 0x08; bond-HB TX uses opcode 170, not this category
- ESPNOW_EVT_WORKER_STATUS @ components/hardwareone/System_ESPNow_Identity.h:123 — retired opcode; comment explicitly 'bit kept for bitmap stability'
- ESPNOW_EVT_METADATA_PUSH @ components/hardwareone/System_ESPNow_Identity.h:124 — bit 0x20; distinct from opcode ESPNOW_V4_TYPE_METADATA_PUSH
- ESPNOW_EVT_TIME_SYNC @ components/hardwareone/System_ESPNow_Identity.h:125 — bit 0x40; TIME_SYNC broadcast is ungated

---

## Two items flagged by a finder but confirmed LIVE (do NOT remove)

- **STREAM_APPEND_DUP** @ `components/hardwareone/System_ESPNow_Files.h:168` — Returned at System_ESPNow_Files.cpp:509 and its value steers caller control flow via the comparison at System_ESPNow.cpp:3935 (DUP falls through to the non-abort path).
- **JOB_RAW** @ `components/hardwareone/System_ESPNow_Tx.h:104` — Referenced by an executing switch-case comparison in runJob (System_ESPNow_Tx.cpp:84), liveness surface #8 (enum used inside a compare). No producer assigns it, so the case body is an unreachable/dead branch — a live symbol with a dead branch, not a dead symbol.

These are recorded so the same false-positive isn't re-flagged later.

---

## Special handling (read before deleting)

- **Reserved event-category bits** (`ESPNOW_EVT_TOPOLOGY/BOND_HEARTBEAT/WORKER_STATUS/METADATA_PUSH/TIME_SYNC`): dead as C identifiers, but their bit *positions* are deliberately reserved for wire/bitmap stability (the wire carries raw numeric masks). Safe to drop the names, but **do not renumber the surviving enumerators** if you keep any.
- **`FILE_SLOT_FAILED`**: the enum value is never assigned, but the diag table `kStates[3]="FAILED"` indexes by literal position. If you remove the enumerator, leave the positional table alone (or fix both together).
- **Stale comments to fix in passing** (not code, but wrong): `espnowtx::getStats` header says "CLI can read via getStats()" (false); `shouldStreamSensorToRemote` says it "replaces the 7-line block" (never adopted); `MESH_MAX_RETRIES` comment points at a non-existent `espnow_system.h`.

---

## Flat checklist for the removal-verification pass

| ✓ | Symbol | Kind | Location | Note |
|---|--------|------|----------|------|
| ☐ | `sendChunkedResponse` | function | `components/hardwareone/System_ESPNow.cpp:907` | Non-static function; only references in the whole corpus are its definition and a prototype in System_ESPNow.h:1154. No caller anywhere. Not |
| ☐ | `v4_send_text` | function | `components/hardwareone/System_ESPNow.cpp:2540` | Only occurrences are the definition (2540), a forward declaration (293), and a comment mention (2547). No caller in any file; no header prot |
| ☐ | `v4_broadcast_topo_request` | function | `components/hardwareone/System_ESPNow.cpp:2284` | Topology-request initiator. Only occurrences are its definition (2284) and a forward declaration (291). No caller anywhere in corpus — devic |
| ☐ | `v4_send_topo_request` | static-function | `components/hardwareone/System_ESPNow.cpp:2269` | Static helper whose only caller is v4_broadcast_topo_request (line 2287), which is itself dead (no caller anywhere). Dead cluster: reachable |
| ☐ | `gLastSentFriendlyName` | static-var | `components/hardwareone/System_ESPNow.cpp:358` | Metadata-tracking global. Single occurrence in entire corpus (its definition) — never read and never written. |
| ☐ | `gLastSentRoom` | static-var | `components/hardwareone/System_ESPNow.cpp:359` | Metadata-tracking global. Single occurrence (definition only) — never read or written. |
| ☐ | `gLastSentZone` | static-var | `components/hardwareone/System_ESPNow.cpp:360` | Metadata-tracking global. Single occurrence (definition only) — never read or written. |
| ☐ | `gLastSentTags` | static-var | `components/hardwareone/System_ESPNow.cpp:361` | Metadata-tracking global. Single occurrence (definition only) — never read or written. |
| ☐ | `espnowCollapsedPeerMessages` | function | `components/hardwareone/System_ESPNow.cpp:15491` | Defined at 15491 with a matching prototype in System_ESPNow.h:1206, but NO caller anywhere in the corpus. grep for 'espnowCollapsedPeerMessa |
| ☐ | `MSG_TYPE_HB` | macro | `components/hardwareone/System_ESPNow.h:53` | V2/V3 JSON-envelope message-type string macro; V4 binary transport replaced it. Zero references anywhere in corpus (macro name would appear  |
| ☐ | `MSG_TYPE_ACK` | macro | `components/hardwareone/System_ESPNow.h:54` | Vestigial V2/V3 envelope type. Zero refs; only sibling MSG_TYPE_BOOT is still used by v2_init_envelope. |
| ☐ | `MSG_TYPE_MESH_SYS` | macro | `components/hardwareone/System_ESPNow.h:55` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `MSG_TYPE_RESPONSE` | macro | `components/hardwareone/System_ESPNow.h:56` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `MSG_TYPE_STREAM` | macro | `components/hardwareone/System_ESPNow.h:57` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `MSG_TYPE_FILE_STR` | macro | `components/hardwareone/System_ESPNow.h:59` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `MSG_TYPE_CMD` | macro | `components/hardwareone/System_ESPNow.h:60` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `MSG_TYPE_TEXT` | macro | `components/hardwareone/System_ESPNow.h:61` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `MSG_TYPE_USER_SYNC` | macro | `components/hardwareone/System_ESPNow.h:62` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `MSG_TYPE_FILE_BROWSE` | macro | `components/hardwareone/System_ESPNow.h:63` | Vestigial V2/V3 envelope type. Zero refs. |
| ☐ | `PAYLOAD_CMD` | macro | `components/hardwareone/System_ESPNow.h:66` | V2/V3 payload-type string macro; superseded by V4. Zero refs (macro name would appear if used). |
| ☐ | `PAYLOAD_TOPO_REQ` | macro | `components/hardwareone/System_ESPNow.h:67` | Vestigial V2/V3 payload type. Zero refs. |
| ☐ | `PAYLOAD_TOPO_RESP` | macro | `components/hardwareone/System_ESPNow.h:68` | Vestigial V2/V3 payload type. Zero refs. |
| ☐ | `PAYLOAD_QUERY` | macro | `components/hardwareone/System_ESPNow.h:69` | Vestigial V2/V3 payload type. Zero refs. |
| ☐ | `PAYLOAD_STATUS` | macro | `components/hardwareone/System_ESPNow.h:70` | Vestigial V2/V3 payload type. Zero refs. |
| ☐ | `PAYLOAD_TIME_SYNC` | macro | `components/hardwareone/System_ESPNow.h:71` | Vestigial V2/V3 payload type. Zero refs. |
| ☐ | `MESH_ACK_TIMEOUT_MS` | macro | `components/hardwareone/System_ESPNow.h:253` | Defined alongside the mesh retry-queue constants but never referenced. The retry queue itself uses MESH_RETRY_QUEUE_SIZE; this timeout const |
| ☐ | `MESH_MAX_RETRIES` | macro | `components/hardwareone/System_ESPNow.h:254` | Only appearance is inside a comment in System_ESPNow.cpp:371 (a note that it 'is now in espnow_system.h'); no executable reference. Retry lo |
| ☐ | `MESH_DEDUP_WINDOW` | macro | `components/hardwareone/System_ESPNow.h:269` | Defined next to MESH_DEDUP_SIZE (which is used to size gMeshSeen[]) but the window constant itself is never referenced. |
| ☐ | `bondRoleStr` | function | `components/hardwareone/System_ESPNow.h:80` | Inline helper defined in header; no caller anywhere. Siblings isBondMaster/isBondWorker are heavily used but this formatter has zero call si |
| ☐ | `shouldStreamSensorToRemote` | function | `components/hardwareone/System_ESPNow.h:1088` | Inline helper whose comment claims it 'replaces the identical 7-line block copy-pasted into every sensor task loop', but no sensor loop (nor |
| ☐ | `MeshTopoPeer` | struct | `components/hardwareone/System_ESPNow.h:156` | Only reference is as the element type of MeshTopoNode::peers (std::vector<MeshTopoPeer>), also in this header. MeshTopoNode's only instance  |
| ☐ | `ESPNOW_V4_FLAG_ENCRYPTED` | flag | `components/hardwareone/System_ESPNow_Wire.h:177` | Self-documented DEPRECATED/VESTIGIAL legacy LMK-era bit. Never set since the Phase 3.5 LMK rip; AEAD confidentiality is signalled by ESPNOW_ |
| ☐ | `ESPNOW_V4_FLAG_COMPRESS` | flag | `components/hardwareone/System_ESPNow_Wire.h:182` | Marked 'future'. Never set on TX and never compared on RX anywhere in the corpus. No #if gate — an unimplemented future earmark, not config- |
| ☐ | `ESPNOW_V4_FLAG_PRIORITY_HIGH` | flag | `components/hardwareone/System_ESPNow_Wire.h:189` | Marked 'Phase 5: bump above retry queue'. Never set or tested anywhere in the corpus. Not behind any config/board gate — an unimplemented fu |
| ☐ | `ESPNOW_V4_HEADER_LEN` | macro | `components/hardwareone/System_ESPNow_Wire.h:29` | Header-length macro is never used in code. The header.headerLen field is assigned via sizeof(EspNowV4Header) (System_ESPNow.cpp:1552,1653,20 |
| ☐ | `ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT` | macro | `components/hardwareone/System_ESPNow_Wire.h:47` | Derived size-cap macro (186 bytes) never referenced anywhere in the corpus. Its inputs (ESPNOW_V4_MAX_PAYLOAD, ESPNOW_V4_BROADCAST_AUTH_TAG_ |
| ☐ | `sendStatusGet` | function | `components/hardwareone/System_ESPNow_Sessions.cpp:738` | Public single-entry lookup with zero call sites. Header comment explicitly says 'Has NO callers'; web UI uses the sendStatusSlotCount/sendSt |
| ☐ | `rekeyTxSeqAtInit` | field | `components/hardwareone/System_ESPNow_Sessions.h:86` | Struct field is write-only across the entire corpus: assigned in sessionMarkRekeyInitiated and zeroed in apply/abort (Sessions.cpp:807,788,8 |
| ☐ | `ESPNOW_EVT_TOPOLOGY` | enum-value | `components/hardwareone/System_ESPNow_Identity.h:121` | Named enum constant with ZERO code references. Grep across components/hardwareone/*.{cpp,h} and main/*.cpp finds only its own definition. Su |
| ☐ | `ESPNOW_EVT_BOND_HEARTBEAT` | enum-value | `components/hardwareone/System_ESPNow_Identity.h:122` | Named enum constant never referenced in code. Only HEARTBEAT and SENSOR bits are passed to peerIdentityWantsEvent; wire uses raw numeric mas |
| ☐ | `ESPNOW_EVT_WORKER_STATUS` | enum-value | `components/hardwareone/System_ESPNow_Identity.h:123` | Named enum constant never referenced in code. Its own comment says 'retired opcode (ex-WORKER_STATUS) — bit kept for bitmap stability', i.e. |
| ☐ | `ESPNOW_EVT_METADATA_PUSH` | enum-value | `components/hardwareone/System_ESPNow_Identity.h:124` | Named enum constant never referenced in code. peerIdentityWantsEvent only ever gates on HEARTBEAT/SENSOR; wire carries raw numeric masks. Bi |
| ☐ | `ESPNOW_EVT_TIME_SYNC` | enum-value | `components/hardwareone/System_ESPNow_Identity.h:125` | Named enum constant never referenced in code. Only HEARTBEAT/SENSOR categories are compared; wire uses raw numeric masks. Bit value document |
| ☐ | `FILE_SLOT_FAILED` | enum-value | `components/hardwareone/System_ESPNow_Files.h:60` | Enum value is never assigned, compared, or emitted anywhere in the corpus. Only FILE_SLOT_FREE/RECEIVING/COMPLETING are used as states; stre |
| ☐ | `RemoteSensorStatus` | struct | `components/hardwareone/System_ESPNow_Sensors.h:32` | Struct type is never instantiated or referenced anywhere in the corpus. Status changes are broadcast as JSON (broadcastSensorStatus) and con |
| ☐ | `espnowtx::getStats` | function | `components/hardwareone/System_ESPNow_Tx.cpp:230` | Public accessor with no callers. Header comment claims 'CLI / other code can also read them via getStats()' but no code does — the txTask st |
| ☐ | `oledEspNowGetMainMenuItemCount` | function | `components/hardwareone/OLED_ESPNow.cpp:654` | Public function (returns ESPNOW_MENU_ITEM_COUNT) with no caller anywhere. Menu bounds are enforced directly with ESPNOW_MENU_ITEM_COUNT in o |
| ☐ | `oledEspNowFormatMac` | function | `components/hardwareone/OLED_ESPNow.cpp:1579` | MAC-formatting helper with no caller in corpus; only definition + header prototype. Not referenced via any indirect surface. |
| ☐ | `oledEspNowValidateDevicePtr` | function | `components/hardwareone/OLED_ESPNow.cpp:1635` | Buffer-safety validator never called; its sibling oledEspNowValidateMessagePtr IS used (line 1505) but this one has zero callers. Not in any |
| ☐ | `oledEspNowSendBrowseRequest` | function | `components/hardwareone/OLED_ESPNow.cpp:2337` | Part of an unwired remote-file-browse feature. No caller anywhere; the input handler that would trigger a browse is itself dead. Not a funct |
| ☐ | `oledEspNowDisplayRemoteFiles` | function | `components/hardwareone/OLED_ESPNow.cpp:2352` | View renderer with no dispatch: there is no ESPNOW_VIEW_* enum value for remote files, so the oledEspNowDisplay() switch can never reach it. |
| ☐ | `oledEspNowHandleRemoteFilesInput` | function | `components/hardwareone/OLED_ESPNow.cpp:2409` | Input handler for the nonexistent remote-files view; no ESPNOW_VIEW_* enum routes to it and oledEspNowHandleInput never calls it. Zero calle |
| ☐ | `storeRemoteFileBrowseResult` | function | `components/hardwareone/OLED_ESPNow.cpp:2438` | Declared under #if ENABLE_ESPNOW as 'callable from the ESP-NOW handler even when OLED disabled', but System_ESPNow.cpp never calls it and th |
