G2/R1 Protocol Schemas — FlutterApp RE Reference
==================================================

These `.proto` files were reconstructed by hand from the generated Dart
classes in `docs/FlutterApp-main/lib/src/proto/g2/*.pb.dart`.

**Provenance & trust level**

`docs/FlutterApp-main/` is a **community reverse-engineering effort**, not
the official Even Realities companion app. Treat its claims with care:

- ✅ **Probably accurate**: protobuf field numbers and enum integer values.
  These come from the base64 `FileDescriptorProto` bytes embedded in the
  `*.pbjson.dart` files, which are real wire-level data — likely dumped
  from the official APK or device firmware.
- ⚠️ **Possibly inferred**: human-readable names (`UI_HEALTH_APP_ID`,
  `EvenAIDataPackage`, message field names like `magicRandom`). The RE
  authors named these based on context; some may be guesses.
- ❌ **Explicitly best-effort**: R1 ring envelope framing. The FlutterApp
  authors flag this themselves in their `README.md:49-52` and
  `AGENTS.md:336`. Treat R1 wire format as a starting hypothesis to
  validate against your own packet captures.

**Firmware vs FlutterApp — who wins on conflicts**

When the firmware has empirical on-the-wire observations that disagree
with FlutterApp's RE, the firmware data is more trustworthy. Documented
disagreements:

- **SID 0x0E**: FlutterApp infers `UI_HEALTH_APP_ID`. Firmware sees
  widget-transform-shaped traffic (`System_G2_Protocol.h:98-100`).
  → trust firmware empirical.
- **SID 0x80**: FlutterApp infers `UX_DEVICE_SETTINGS_APP_ID` carrying
  `DevCfgDataPackage`. Firmware sees `08 06 10 <varint> 2A 00`-shaped
  traffic and labels it heartbeat-style (`System_G2_Protocol.h:101-104`).
  Note: those bytes happen to fit DevCfgDataPackage as
  `cmd=6 (RING_CONNECT_INFO), magicRandom=*, ringInfo={}`, so FlutterApp
  may be correct here — but the firmware brick blocklist
  (`G2_Glasses.cpp:9105-9142`) should remain in place until verified
  on a device that can be physically reset.
- **R1 pairAuth**: FlutterApp sends payload `0x01` and gets OK. Firmware
  notes from a separate RE thread (`G2_Ring.h:19`) say pairAuth requires
  a server-issued pkey. Could be firmware-version-dependent or the
  FlutterApp pairAuth is a no-op the device acks. Stay cautious.

**File index**
- `common.proto`              — eErrorCode shared across packages
- `service_id_def.proto`      — SID enum (envelope byte 6)
- `dev_pair_manager.proto`    — AuthMgr, RingInfo, PipeRoleChange (sid=128 sub-msgs)
- `dev_settings.proto`        — TimeSync, BaseConnHeartBeat, AudControl (sid=128 sub-msgs)
- `dev_config_protocol.proto` — DevCfgDataPackage wrapper + eDevCfgCommandId (sid=128)
- `even_ai.proto`             — EvenAIDataPackage wrapper + sub-messages (sid=7)
- `EvenHub.proto`             — EvenHub_Cmd_List, OsEventTypeList (sid=224)
- `efs_transmit.proto`        — File-service enums (sid=196..199)
- `ota_transmit.proto`        — OTA enums (sid=192..195)
- `r1_ring_enums.proto`       — R1 ring sub-cmd / status enums (best-effort)
- `ring.proto`                — RingDataPackage / RingRawData / RingEvent
                                 (sid=144/145 — ring-data delivered via glasses
                                 once the bridge is up)
- `G2_RE_Reference.h`         — C header: SID + opcode tables, with firmware
                                 cross-references and conflict notes
- `R1_RE_Reference.h`         — C header: R1 envelope reference + sub-cmds,
                                 with prominent pkey + best-effort warnings
- `Ring_Bridge_Sequence.h`    — **Implementation guide for getting ring data
                                 via the glasses (the no-pkey path)**. Phase
                                 1 (glasses AUTH/role/time) → Phase 3
                                 (RING_CONNECT_INFO) → Phase 4 (receive
                                 telemetry on sid=144/145). Includes a
                                 lowest-risk validation order that avoids
                                 the brick block until proven safe.

**Bridge-related gotchas worth flagging up front**

- **Time-zone encoding is different on G2 vs R1**: G2 `TIME_SYNC` uses
  quarter-hours (minutes/15); R1 `systemTime` uses raw minutes. They look
  interchangeable; they aren't.
- **MAC byte-reversal is direction-dependent**: G2 `RING_CONNECT_INFO`
  expects the ring MAC REVERSED. R1 `advStart` expects the G2 MAC in BLE
  order, NOT reversed. Easy to swap accidentally.
- **The ring direct connection may be skippable**: FlutterApp's R1Manager
  comment at lib/src/services/r1_manager.dart:258-261 says
  RING_CONNECT_INFO alone may be enough to bring up the bridge — if true,
  you can sidestep the pkey question entirely. See Ring_Bridge_Sequence.h
  Phase 2 / quick-start.

**Why these are documentation only**

The firmware does not use nanopb or codegen; it hand-rolls proto3 wire
encoding via `g2PbWrite*` primitives in `components/hardwareone/System_G2_Protocol.h`.
Use these `.proto` files to verify hand-written field numbers against a
second data point. The C headers provide grep-able names that are
distinct from existing `G2_SID_*` / `G2_CMD_*` defines so cross-referencing
firmware code against FlutterApp captures is unambiguous.

Reconstructed 2026-05-01 from the FlutterApp-main snapshot at the time of
import. Re-derive from `lib/src/proto/g2/*.pb.dart` if FlutterApp updates.

### Hijack list updates (hardwareone-idf firmware)

Companion apps in **`docs/evenrealities_rev_share-main/`** (Python EvenHub
demos) and **`docs/FlutterApp-main/`** (Dart EvenHub) drive the same
`sid=0xE0` **EvenCore** stream: `CreateStartUpPage`, `RebuildPage`,
`ShutdownPage`, etc. Protobuf layout for those commands is what the
`*.proto` files here help validate.

**Operational policy in this repo’s C++ client** (`components/hardwareone/G2_Glasses.cpp`):

- Treat **SHUTDOWN → 500 ms → CREATE** as the always-correct way to change
  hijack **content class** (list ↔ text ↔ compound) or **list row count**.
- Use **`Cmd=7` REBUILD-list** only for **pure list → pure list** swaps with
  the **same number of rows** and an intact right-temple container; see
  `HijackListPageShape` + `gLastHijackListRowCount` beside `pageSwapJobBody`.
- Reset that cache on **DISPLAY_OFF**, **SYSTEM_EXIT**, **R BLE disconnect**,
  **plugin-silent (R)**, and `g2NoteContainerCleared`.

Authoritative narrative: **`docs/G2_PROTOCOL.md`** → *REBUILD-list vs
SHUTDOWN+CREATE (hijack page swaps)*.
