# R1 health on ring firmware 2.3.2.x: what the stock app does

Research date: 2026-10-18. Read-only. Nothing was flashed, sent to a device or
committed.

This note answers one question: how does the Even app 2.3.2 (build 243) ask an
R1 ring on firmware 2.3.2.x for health data, and what would HardwareOne need to
admit health features for its exact profiles `2.3.2.0007` and `2.3.2.0008`?

Privacy: this file describes layouts only. It contains no Bluetooth addresses,
serial numbers, keys, body measurements or health values. The capture and the
decompiled app stay under `tools/p4/companion/private/` (git-ignored).

## Summary

1. **The 2026-10-15 capture already has a full health sync from the 0007
   ring.** `timeline.txt` only names module-1 (system) frames, so it looked as
   if the capture had setup traffic only. Re-decoding module 2 shows the stock
   app (2.3.1 build 241) pulling HR, HRV, SpO2, sleep and activity DAILY pages
   from the `2.3.2.0007` ring and acknowledging every page with `packetAck`.
   Every frame has a valid CRC32. Every HR/HRV/SpO2/activity page matches
   HardwareOne's 2.2.9 exact-length parsers. Activity days arrive as
   3-notification frames, framed the way HardwareOne's 2.2.7 reassembler
   already expects.
2. **Build 243 uses the same wire layouts as HardwareOne's 2.2.9 code** for
   the daily query, the HR/SpO2/HRV/activity daily pages, `packetAck`, the
   health-collection setting and the low-power setting. The differences are
   listed in section 5. The one that matters for 2.3.2 is `deviceStatus`
   byte 1: build 243 reads it as **charge status**, not wear.
3. **No firmware-version gate** guards any health request in build 243 (see
   section 4).
4. **The 0008 ring was captured on 2026-10-18 (section 7)** with build 243
   doing a full sync. It speaks the same dialect as 0007: 115 CRC-valid
   frames, and all 16 HR/HRV/SpO2 pages and 6 activity frames pass
   HardwareOne's exact 2.2.9 checks. Activity days of 92 and 72 records came
   as 3-part frames. This time the ring's clock was set, so every timestamp is
   a real epoch.
5. **Verdict.** For **both** `2.3.2.0007` and `2.3.2.0008`, stock-app code
   plus a matching capture from each exact firmware support admitting:
   DAILY HR/HRV/SpO2/activity, activity reassembly, history-gated
   `packetAck`, health collection ON, and device-status battery. On 0008 the
   low-power GET is also captured. Sleep parsing, point/measure, low-power
   SET, user info, wear status and the new 2.3.x commands should stay off.

## 1. Sources and method

| Source | What was used |
|---|---|
| Decompiled app 2.3.2 build 243 (blutter, `ring-2327/even-2.3.2-build243/blutter-out/asm/`) | `even_connect/ring1/` request builders, enums and models; `even/pages/main_screen/tab_page/health/service/health_glasses_push_service.dart` (sync orchestration and parser call sites) |
| Capture `ring-2327/btsnoop_hci.log` (app 2.3.1 build 241, ring `2.3.2.0007`, 2026-10-15) | Re-decoded with a throwaway read-only script that reassembles multi-notification frames, checks CRC32 and CRC-16/MODBUS, and prints structure only (lengths, counts, modes, invariants) |
| HardwareOne | `System_R1_Protocol.cpp/.h`, `G2_Ring.cpp`, `G2_Health.cpp`, host harnesses `r1_protocol_harness.cpp` and `ring_daily_ack_harness.cpp` |

Enum wire values were read from the enum `subCmd` / `cmd` / `fromByte`
switches in `ble_ring1_enum.dart`, not from the enum declaration order (they
differ).

## 2. Opcode map in build 243

Module wire values: system `0x01`, health `0x02`, sport `0x03`, testable
`0x7F`. These match HardwareOne.

| Module / cmd | subCmd (wire) | Name in app | HardwareOne |
|---|---|---|---|
| system / 0x00 | 0x01 deviceStatus, 0x02 deviceInfo, 0x03 wearStatus, 0x04 userInfo, 0x05 systemTime, 0x06 touchStatus, 0x07 touchSwitch, 0x08 pairAuth, 0x09 otaStart, 0x0A advStart, 0x0B getAlgoKeyStatus, 0x0C setAlgoKey, 0x0E healthSettingsStatus, 0x0F systemSettingsStatus, 0x10 deviceSn, 0x11 nvRecover, 0x12 powerControl | same | same |
| system / 0x00 | **0x13 pairDelete** | new | not defined |
| system / 0x00 | 0x7E packetAck, 0x7F heartbeatPack, 0x80, 0x81, 0x82 | same | same |
| system / 0x00 | **0x84 deleteRingInfo** | new | not defined |
| health / 0x01 heartRate, 0x02 spo2, 0x03 temperature, 0x04 hrv | 0x01 daily, 0x02 point, 0x03 measure | same | same |
| health / 0x05 activity | 0x01 daily, **0x02 allDayActivity** | new 0x02 | 0x02 is "point" |
| health / 0x06 sleep | 0x01 daily, **0x02 forceAwake** | new 0x02 | 0x02 is "point" |
| sport / 0x07 sportRunCtrl | 0x00 start, 0x01 stop | new | not defined |
| sport / 0x08 sportRunData | sportRunData | new | not defined |
| health / **0x7F** healthSetting | 0x01 reportEnable | new | `R1_CMD_HEALTHSET` is `0x07` (unused) |

So in 2.3.x, `health/activity/0x02` and `health/sleep/0x02` are not "point".
HardwareOne's diagnostic `buildHealthQuery(cmd, R1_SUB_POINT)` must never be
allowed for those two commands on a 2.3.2 profile. Today it is only allowed on
2.2.7.

## 3. Per-operation detail (build 243)

Status byte: bit 0 type (0 notify, 1 ack), bit 1 method (0 get, 1 set),
bits 2–3 ack (0 ok, 1 error, 2 refuse, 3 notSupport). Identical to
HardwareOne.

### 3.1 Daily query, per metric

* Builder: `BleRing1CmdHealthExt.getDailyData(cmd)`
  (`ble_ring1_cmd_proto.dart`, 0x1d95760).
* Frame: module health, the given cmd, subCmd daily (0x01), status
  notify/get/ok, **empty payload**, default timeout.
* Ring behaviour (capture): the ring first answers with `ack/set/ok`, same
  serial, empty payload. It then sends one or more DATA notifies with
  `notify/set/ok` and **its own serial counter**. It sends every page of the
  query without waiting for per-page acknowledgements.
* Metrics the app syncs: HR (1), HRV (4), SpO2 (2), sleep (6), activity (5).
  Temperature daily is skipped by the app: a log key
  `ring_daily_skip_reserved_temperature` sits next to the parser call.
* HardwareOne: `buildHealthQuery(profile, cmd, R1_SUB_DAILY)` builds the same
  bytes.

### 3.2 Daily reply layouts

All little-endian. One parser handles HR/SpO2/HRV/activity:
`BleRing1HealthDailyFactory._parseDaily` (0x220fe6c).

**Common prefix (7 B):** `[0] count u8`, `[1..2] tz minutes i16`,
`[3..6] dayStart i32`.

**HR, SpO2 (`parseCommonDaily`, items 4 B, `withLatestData: true`):**
`prefix | [7..10] latestTs i32 | [11] latest u8 | count × {slot u8, avg u8, max u8, min u8} | [optional] trailer u32`

**HRV (items 7 B, latest is i16):**
`prefix | [7..10] latestTs i32 | [11..12] latest i16 | count × {slot u8, avg i16, max i16, min i16} | [optional] trailer u32`

The app picks the 7-byte item form for both HRV and temperature (the call
sites test `cmd == temperature || cmd == hrv`). Temperature is skipped anyway.

**Activity (`parseActivityDaily`, items 7 B, no latest fields):**
`prefix | count × {slot u8, steps i16, activeKcal i16, totalKcal i16} | [optional] trailer u32`
(item fields from the `actKcal` / `allKcal` getters; slots are 10-minute).

**Length rule in the app:** `len >= header + count × itemSize`. The trailer is
read only when 4 more bytes exist. Items are not range-checked. Anything
longer is ignored.

**Sleep (`BleRing1HealthSleep.fromBytes`, 0x2211150):** minimum 32 bytes.

| Offset | Type | Meaning (where known) |
|---|---|---|
| 0 | u8 | sleep type: 1 long, 2 short |
| 1–7 | 7 × u8 | unnamed |
| 8–9 | u16 | unnamed |
| 10–11 | i16 | timezone minutes: −240 in the 0008 capture; 0 on 0007, where the records predate the time sync |
| 12–15 | u32 | start timestamp (debug page labels it `startTs`) |
| 16–19 | u32 | end timestamp (`endTs`) |
| 20–29 | 5 × u16 | unnamed |
| 30–31 | u16 | stage count N |
| 32… | N × 3 B | stage {type u8: 0 awake, 1 rem, 2 light, 3 deep; duration u16} |
| 32+3N | u32, optional | trailer |

In every long-sleep record of the capture, the sum of stage durations × 30 s
equals end − start, so the stage unit is 30-second epochs. Short-sleep records
had zero stages.

**1970 / reboot correction:** after parsing, the app runs
`_fixRebootDailyForCommon`, `_fixRebootDailyForActivity` and `_fixRebootSleep`.
When the item timestamps fall in early 1970 (`_isReboot1970Range`), the app
shifts `dayStart` and `latestTs` (or sleep start/end) by
`phone now − trailer` (`_shiftBaseTimestampToNow`). There is also a debug page,
`health_1970_offset_debug_page.dart`. This is how the app dates pages that the
ring recorded before it knew the wall time.

### 3.3 Multi-frame transport and packetAck

* **Multi-notification frames (capture):** byte 0 of each notification is a
  countdown: `2`, `1`, `0` for a 3-part frame. Every part repeats the
  whole-model CRC32 in bytes 1–4. The model header and declared length appear
  once, in the first part. With an MTU of 247, activity days of 92 and 73
  records came as 3-part frames of 667 and 534 model bytes. HardwareOne's
  reassembler (`G2_Ring.cpp`, "countdown decrements to 0 on the last
  fragment") is this scheme.
* **packetAck** (`BleRing1SystemPackageAck.toBytes`, 10 bytes):
  `[0] module`, `[1] cmd`, `[2] subCmd`, `[3] 0`, `[4..5] received serial u16`,
  `[6..9] 0`. It is sent as system/0x00/0x7E, status ack/get/ok. The capture
  shows exactly this for every HR, HRV, SpO2, sleep and activity page, sent
  0.06–1.3 s after the page arrived and interleaved with other traffic. The
  app sent the activity query before it had acknowledged the last four sleep
  pages, so acks do not gate the next query.
  HardwareOne's `buildPacketAck` builds the same bytes.
* **The ack consumes data on the ring.** In the capture's second round, one
  minute later, each metric came back with only the still-open hour (count=1)
  and one sleep record. Whoever acknowledges a page takes it away from the
  Even app. HardwareOne's 2.2.9 rule acknowledges only pages the Health
  history model accepted. That rule should carry over unchanged.
* For where the app decides to send the ack and how it reassembles frames,
  see section 4. In short: it acknowledges every health notify, and its
  reassembler uses the same countdown contract.

### 3.4 Point and measure

* `health/{hr,spo2,temp,hrv}/point` and `/measure` exist in the enum (0x02,
  0x03), but build 243 has **no builder or sender for either**. Point samples
  are only consumed when the ring sends them on its own, and the app
  acknowledges them.
* Point notify parser (`BleRing1HealthPoint.fromBytes`): minimum 8 bytes.
  `[0..1] value i16`, `[2..5] ts i32`, `[6] state` (0 fail, 1 success,
  2 notWear, 3 notStationary, 4 timeout), then `extra` = i16 at `[7..8]` if
  `len >= 10`, else u8 at `[7]`. HardwareOne switches to i16 at `len >= 9`.
* No point or measure traffic appears in the 0007 capture.

### 3.5 Health collection on/off

* `setHealthEnable(bool)` → `BleRing1HealthSettingsStatus.forHealthEnable` →
  system/0x00/0x0E, status notify/**set**/ok, timeout 3000 ms.
* Payload, 12 bytes: `[0..3] epoch now (s, UTC) u32`, `[4] enable (1/0)`,
  `[5..11] 7 reserved zeros`. ON and OFF use the same builder.
* Capture: app 2.3.1 sends the ON form right after pairAuth. The ring answers
  `ack/set/ok`, empty.
* HardwareOne's `buildHealthCollectionSet` builds the same bytes.

### 3.6 Low power (system settings)

* system/0x00/0x0F. Model `BleRing1SystemSettingsStatus`, 12 bytes:
  `[0..3] epoch u32`, `[4] switchType u8`, `[5] switchStatus u8`,
  `[6..11] swReserved`. `fromBytes` requires at least 12 bytes. GET sends an
  empty payload and parses the reply; SET sends the 12-byte form.
* Same layout as HardwareOne's `buildLowPowerSet` and
  `r1ParseLowPowerStatus`. HardwareOne also requires the reserved bytes to be
  zero; the app does not check them.
* Not present in the 0007 capture.

### 3.7 User info

* Capture: after `systemTime`, the **ring** sends `userInfo`
  (system/0x04, notify/set/ok, empty payload). That is a request. The app
  answers with system/0x04, notify/**get**/ok, 12-byte payload
  (gender, age, height u16, weight u16, 6 reserved), and the ring acks.
* Same layout as HardwareOne's `r1ParseUserInfo`. HardwareOne deliberately
  has no builder and should keep it that way. HardwareOne never answers the
  request, and its setup still completed on 0007 (2026-10-16) and 0008
  (2026-10-18). Whether the missing answer changes the ring's calorie or
  other algorithms is unknown.

### 3.8 Wear status

* system/0x00/0x03, `getWearStatus`, response 1 byte. Codes: 0 unknown,
  1 notWear, 2 wear, **3 possiblyWear** (new). HardwareOne records 3 as its
  own state, shown as "maybe" (section 8e); values above 3 are ignored.
* The app sends the query only during onboarding. On 0008, the ring itself
  **pushes** a 1-byte `notify` on every wear change (charger/desk/finger
  capture, section 7): 1 when it went on the charger and again when it came
  off, 3 on the desk a few minutes later, and 2 when it went back on a finger.
  That push is the 2.3.2 wear source.

### 3.9 Device status

* system/0x00/0x01, empty GET. The reply is 7 bytes in the capture.
* Build 243 (`BleRing1SystemStatus.fromBytes`): `[0] u8` (unnamed in the
  app; HardwareOne already reads it as battery %),
  **`[1] charge status`** (0 unknown, 1 charging, 2 notCharging, 3 full). When
  charging, it also parses a `BleRing1ChargeInfo` from the bytes after that.
* HardwareOne reads `[1]` as wear (0/1/2) for 2.2.7 and 2.2.9. On a worn ring
  off the charger both readings give `2`, which is probably why this went
  unnoticed. For 2.3.2, ingest the battery byte only, or record `[1]` as
  charge status. Do not feed it to `ringNoteWear`.
* **Confirmed on 0008:** `[1]` was 1 on the charger, then 2 both on the desk
  and on a finger. The wear reading would have given 1 on the desk. While
  charging, the ring also pushes the 7-byte status as a `notify` every ~30 s
  (byte 2 = 0 in those pushes; GET replies have byte 2 = 1; meaning unknown,
  and it is not wear).

### 3.10 New in 2.3.x (leave off)

* `health/sleep/forceAwake` (cmd 6, sub 2), sent notify/**set**, empty,
  timeout 3000. App 2.3.1 sent it once at setup. No reply was seen within the
  capture.
* `health/activity/allDayActivity` (cmd 5, sub 2). Parsed from ring-initiated
  notifies (`BleRing1HealthActivityAllDay.fromBytes`, at least 12 bytes:
  i16, u32, u32, i16, i16; names unknown).
* `healthSetting(0x7F)/reportEnable(0x01)`, `pairDelete (0x13)`,
  `deleteRingInfo (0x84)`, sport run start/stop/data.
* In the capture, after a long ring-to-phone bulk transfer (a log export, not
  health), the app sent `deleteRingInfo` with payload `01`.

## 4. How build 243 orchestrates the sync, and version gates

Paths are under `blutter-out/asm/`.

**Driver.** `HealthGlassesPushService.fetchAllHistoryDataFromRing`
(`even/pages/main_screen/tab_page/health/service/health_glasses_push_service.dart`)
runs one `HealthRingHistorySyncRound` (`health_ring_history_sync_round.dart`).
The round's send step is `getDailyData(cmd)`, reached through
`HealthRingGateway` in `health_glasses_push_gateways.dart`. A step counts as
sent when the ring's request ack has `ack == ok`. The default command timeout
is 1000 ms.

**One round:**

* Fixed order: **heartRate → hrv → spo2 → sleep → activity**. There is no
  temperature step.
* 500 ms gap between steps (Duration 500 000 µs, checked in `objs.txt`).
* A failed request ack gets one retry after 500 ms ("ack_failed_retry"); a
  second failure is logged and the round moves on.
* 10 s round timer ("timeout_or_forceStop").
* The round completes when HR, HRV and SpO2 data have all arrived. Sleep and
  activity are sent but not waited for.

The capture shows exactly this order, in both rounds.

**Gates before a round:** a signed-in account, ring connected, health
tracking enabled, ring not busy (OTA or log transfer), 60 s debounce between
attempts, a 10-minute throttle since the last successful sync (skipped for
"quick" triggers), and no round already running.

**Triggers:**

| Trigger | Notes |
|---|---|
| Ring session connected | Quick. Kicks the round, then `forceAwake` if the app is in the foreground |
| Home controller after a ring change | Quick; collapsed by the debounce. Followed by deviceInfo, charge status and algo-key status |
| Account bound / privacy confirmed / health tracking turned on | Quick |
| App to foreground | Throttled. Then `forceAwake` |
| Hourly timer | Only while backgrounded and connected |
| Health page entered | Throttled |
| After a round, if a pull was skipped meanwhile | Deferred retry |

**Other requests and when they go out:**

* `forceAwake` (sleep/0x02, set, timeout 3000 ms): only while in the
  foreground, with a **1 h cooldown**, on connect and on foreground.
* `setHealthEnable` (system/0x0E): `HealthTrackingRingSyncService` sends it
  on init, on ring connect, on server confirmation and when the user changes
  the setting. Both ON and OFF are sent.
* `setUserInfo`: from the home controller and the global service (profile
  data).
* `getWearStatus`: onboarding only.
* `get/setSystemSettingsStatus` (low power): only from the device settings
  page's low-performance-mode control.
* `deleteRingInfo(eventPoint)`: only after a ring event-log pull. That pull is
  gated on firmware ≥ 2.0.3.0004, at most every 5 h, and battery state, and is
  scheduled after a successful cloud sync.
* `pairDelete`: only when removing the device.
* Point, measure and `reportEnable`: **no sender exists**; they appear only
  in the enums.

**packetAck in the app:** `BleRing1Service` acknowledges every single-frame
notify whose cmd is a health cmd, whatever the subCmd (point pages included),
plus `sportRunCtrl`. It also acknowledges every reassembled multi-frame
health model whose payload validated. The ack is fire-and-forget (timeout 0).

**Reassembly in the app** (`BleRing1CmdPrivateExt._handleMultiPacket`): a
notify is multi-part when byte 0 ≠ 0 or a buffer is already open. A CRC32
change drops the stale buffer. Parts are appended until byte 0 == 0. The app
then requires `first.byte0 + 1 == part count`, concatenates the parts in
arrival order, checks the whole-model CRC32, and parses the result as one
model. Same contract as HardwareOne's countdown reassembler.

**Firmware-version gates:** none on any health path. The health service, the
ring1 command layer and the ring OTA resolver contain no `compareVersion`
call and no version literal (checked independently with grep). `deviceInfo`
feeds the firmware-version cache and update UI only. The version comparisons
that do exist are for OTA and forced updates ("2.0.3.0004"), the event-log
pull ("2.0.3.0004"), and a contextual-menu tutorial ("2.2.9", not
health-related). So build 243 speaks the same health dialect to every ring
firmware it can pair with, including 2.3.2.0007 and 2.3.2.0008.

## 5. Side by side with HardwareOne's 2.2.9 layouts

| Operation | Build 243 / 0007 capture | HardwareOne 2.2.9 | Same? |
|---|---|---|---|
| Daily request | health/cmd/0x01, notify/get/ok, empty | `buildHealthQuery(..., R1_SUB_DAILY)` | Same |
| Request ack | ack/set/ok, same serial, empty | Transaction expects it | Same |
| Data notify status | notify/set/ok, ring serial | `validateDailyFrame` requires notify/set/ok | Same |
| Prefix | count u8, tz i16, dayStart i32 | `parseDailyPrefix`: count, tz i16, dayStart **u32** | Same bytes; signedness differs only after 2038 |
| HR/SpO2 latest | latestTs i32 @7, latest u8 @11 | latestTs u32 @7, latest u8 @11 | Same |
| HR/SpO2 item | slot, avg, max, min (u8) | slot, average, maximum, minimum | Same |
| HRV latest/item | latest i16 @11; item slot u8 + 3 × i16 | u16 @11; slot + 3 × u16 | Same bytes, signedness only |
| Activity item | slot, steps, actKcal, allKcal (i16) | slot, steps, activeKcal, totalKcal (u16) | Same bytes, signedness only |
| Trailer | optional u32 (used by the 1970 fix-up) | required u32, kept as `trailer` | HardwareOne is stricter |
| Length | `>=` header + items | exact: 16+4n, 17+7n, 11+7n | HardwareOne is stricter (fails closed) |
| Slot checks | none | ≤23 (≤143), unique, min≤avg≤max, total≥active | HardwareOne is stricter |
| Timestamps | 1970-range data is shifted by `now − trailer` | zero-base day = unanchored; latestTs classified epoch / seconds-in-day / unknown | Different. HardwareOne never shifts and drops unanchored pages from history |
| Multi-notification frames | countdown byte 2/1/0, CRC32 repeated | 2.2.7 reassembler, same scheme | Same; 2.3.2 not yet enabled |
| packetAck | module, cmd, sub, 0, serial u16, 4 × 0; ack/get/ok | same 10 bytes | Same |
| Which pages are acked | every parsed page (HR, HRV, SpO2, sleep, activity) | 2.2.9: HR/HRV/SpO2/activity pages the history model accepted; never sleep | Policy differs on purpose |
| Health collection | 12 B: epoch, enable, 7 × 0; ON and OFF | same; 2.2.9 allows ON only | Same layout |
| Low power | 12 B: epoch, type, status, 6 reserved | same | Same layout |
| User info | ring requests; app answers 12 B | decode-only | Same layout, different policy |
| deviceStatus [1] | charge status | wear | **Different** |
| Sleep daily | 32 B header + 3 B stages (+ trailer) | not parsed on 2.2.9 | Not implemented |
| CRC16 | CRC-16/MODBUS over the model with bytes 10–11 zeroed (all capture frames verify) | `r1Crc16` (older community algorithm) | Different; ring tolerates it |

### What the 0007 capture shows (structure only)

* Round 1, after setup: HR, HRV and SpO2 each returned two pages: one
  zero-base page (dayStart 0, tz 0) and one page keyed to local midnight
  (tz −240, `classifyDailyDay` → epoch). Sleep returned 10 records (7 long,
  3 short). Activity returned two 3-part frames (one zero-base, one dated).
* Round 2, one minute later: one page per metric (count 1, the open hour or
  ten-minute slot) and one short-sleep record.
* All pages pass HardwareOne's exact-length formula, slot range, uniqueness,
  ascending order and min≤avg≤max (or total≥active) checks.
* `latestTs`, the trailer and all sleep timestamps were in the 1970 range
  (below 2020, above one day), even after `systemTime`. HardwareOne
  classifies such a `latestTs` as unknown and keeps `latestTimestamp = 0`.
  Dated pages are still anchored by `dayStart + slot`. The trailer was
  identical in both rounds, a minute apart, so it is not simply the ring's
  "now". The app's `now − trailer` shift is therefore only approximate, and
  HardwareOne should not copy it.

## 6. Admission verdict

| Feature (capability helper) | 2.3.2.0007 | 2.3.2.0008 |
|---|---|---|
| DAILY query HR/HRV/SpO2/activity (`HealthQuery`, `SingleFrameDaily`) | Admit: app code + matching capture | Admit: app code + matching capture |
| DAILY query sleep, presence only (`HealthQuery` for SLEEP, no ingestion) | Admit, as on 2.2.9 | Admit. Captured, including the "no new sleep" case (request ack, no page) |
| Activity reassembly (`ActivityReassembly`) | Admit: 3-part frames captured | Admit: 3-part frames captured |
| packetAck for accepted HR/HRV/SpO2/activity pages (`DailyPacketAck`) | Admit with the 2.2.9 history-gated rule | Same |
| Health page refresh (`HealthPageRefresh`) | Admit, composed of the above | Same |
| Health collection ON (`HealthCollectionSet`, true) | Admit: captured | Admit: captured |
| Health collection OFF | App code only (same builder). Admit if wanted; no capture of OFF | Same |
| deviceStatus battery (`DeviceStatusIngestion`) | Admit battery byte 0 only | Same |
| deviceStatus wear | **Do not admit.** Byte 1 is charge status in build 243 | **Do not admit.** Charge status confirmed on the charger |
| Wear from the ring's own wearStatus push | Admit (same firmware family; app code) | Admit: pushes captured (1, 2, 3); 3 = possiblyWear is not recorded |
| Sleep data ingestion | Not yet. Layout known; on 0007 the timestamps are 1970-range and need a dating decision | Not yet. Layout confirmed with epoch times and tz at bytes 10–11; still needs a parser, tests and a dating rule for 1970-range records |
| Point / measure query | No. Not in the capture; sub 0x02 means something else for sleep and activity | Same |
| Point ingestion (passive) | Optional; layout known, nothing captured | Same |
| Low power GET (`LowPower`, read side) | No capture | Captured: 12-byte reply, epoch field 0, switchType 0, reserved 0. `r1ParseLowPowerStatus` accepts it. Admit GET only if the helper is split into read and write |
| Low power SET | No capture; layout matches | Same |
| Wear status query | No. The app sends it only during onboarding; no capture | Same |
| User info | Keep decode-only. No builder (existing policy) | Same |
| forceAwake / allDayActivity / reportEnable / deleteRingInfo / pairDelete / sport | Never from HardwareOne | Same |

## 7. The 0008 capture (2026-10-18)

**How it was taken:**

* The P4 was powered off so it couldn't hold the ring.
* The HCI snoop log was set to Enabled and Bluetooth toggled before the ring
  connected to the Even app (2.3.2 build 243, work profile).
* The user opened the Health tab and refreshed, then waited about 2 minutes.
* `adb bugreport` went into
  `tools/p4/companion/private/console-debug-2026-10-18/ring-2328/`
  (owner-only). Only `btsnoop_hci.log` and `.log.last` were extracted, their
  sha256 hashes are in `hci-sha256.txt`, and the zip's hash and Bluetooth
  member list are in `bugreport-index.json`. **The zip was deleted.**
* The ring link is in `btsnoop_hci.log` (115 CRC-valid R1 frames); `.last`
  holds no ring traffic.

**What it shows (structure only):**

* **Setup:** pairAuth → health collection ON (12 B, enable=1, reserved zero)
  → deviceInfo → forceAwake (no reply, as on 0007) → systemTime. Then the
  ring sends its userInfo request and nvRecover push, and the app sends
  advStart.
* **Round 1:** HR → HRV → SpO2 → sleep → activity, matching section 4. HR,
  HRV and SpO2 each returned **4 pages for 4 consecutive days**, all dated
  (`classifyDailyDay` → epoch, tz −240). Sleep returned 1 short record.
  Activity returned 4 frames: two single, plus two 3-part frames of 92 and 72
  records. Every page got a packetAck.
* **Round 2** (after the Health page refresh): one page per metric with
  count 1, the most recent slot again. The sleep request got only its request
  ack with no page, which is how the ring says "no new sleep".
* All HR/HRV/SpO2 pages pass the exact 16+4n / 17+7n lengths, slot ≤ 23,
  unique, ascending, min ≤ avg ≤ max. All activity frames pass 11+7n,
  slot ≤ 143, total ≥ active.
* `latestTs` is a real epoch on 0008. It is **the same instant on every
  page** (the ring-wide latest sample), not per day: `latestTs − dayStart`
  drops by exactly 86 400 s from page to page.
* The trailer is a small constant (seconds in the ring's uptime domain). It
  was the same in both rounds, a minute apart, and the same across all
  metrics, exactly as on 0007. A plausible reading is "ring uptime when wall
  time was first set". This is unproven.
* Sleep bytes 10–11 hold tz −240 here (0 on 0007, where the record predates
  the time sync), so that field is the timezone.
* The low-power GET (system/0x0F) was answered: 12 bytes, epoch field 0,
  switchType 0, status 1, reserved 0.
* `deviceStatus` byte 1 = 2 and byte 2 = 1 with the ring worn and off the
  charger, same as 0007.
* Not exercised: health collection OFF, low-power SET, wear-status query,
  point/measure.
* After the sync, the app pulled the ring's event log (bulk transfer, not
  health) and sent `deleteRingInfo`, as on 0007. A second connection setup
  near the end of the log is cut off by the bugreport.

**Charger / desk / finger capture (same day, 11:27–11:52 EDT,
`ring-2328-charger/`, HCI logs only, zip deleted):**

| EDT | Ring | deviceStatus byte 1 | byte 2 | Ring's wearStatus push |
|---|---|---|---|---|
| 11:22 | finger | 2 | 1 | — |
| 11:27:39 | onto charger | 1, pushed every ~30 s | 0 | 1 notWear |
| 11:33:27 | off charger, on desk | 2 | 0 | 1 notWear |
| 11:36:10 | desk | — | — | 3 possiblyWear |
| 11:37:21 | desk (reconnect) | 2 | 1 | — |
| 11:42:30 | finger again | — | — | 2 wear |
| 11:52:22 | finger | 2 | 1 | — |

Byte 1 is charge status: 2 on the desk rules out wear. The ring stayed
connected while charging.

**After capture:** set the HCI snoop log back to Disabled. Android keeps its
own copy of the bugreport on the phone, under the shell user's bugreports
folder, until it is deleted. The 2026-10-15 bugreport zip (14.5 MB) is still
in `console-debug-2026-10-15/ring-2327/`.

## 8. Code changes and host tests

**Applied 2026-10-18 (uncommitted, not flashed).** The list below was the
plan. What landed differs from it in these ways:

* **Health collection:** 2.3.2 admits ON only, like 2.2.9. OFF is
  app-code-only.
* **Low power:** the read/write split was not done; low power stays off for
  2.3.2.
* **`strictPassive229`:** left as is, because point ingestion stays off for
  2.3.2.
* **Wear:** the ring's wearStatus push is ingested on 2.3.2 through a new
  `r1ProfileSupportsWearStatusIngestion`. The query builder stays gated by
  `r1ProfileSupportsWearStatus`.
* **Not handled yet:** `ringDeviceStatusPayloadValid` still requires a GET
  reply (ACK) with byte 1 ≤ 2. So the ring's own status pushes while
  charging, and "full" (3), don't refresh the battery.
* **Tests:** host tests pass (`test_r1_protocol --sanitize`,
  `test_ring_daily_ack --sanitize` with 97 checks, `test_r1_history_store`,
  and the eight other tests that read these files). The new end-time, wear
  and deviceStatus checks were confirmed to fail on reverted code. The P4
  build of `p4-phone-pipe-camera-off-test` is clean.

**`System_R1_Protocol.cpp`** (capability helpers only; no layout changes):

* Add `r1Is232Health(profile)` = 0007 or 0008 (both captured).
* `r1ProfileSupportsHealthCollectionSet`: 232 → `enabled` (and OFF if
  wanted).
* `r1ProfileSupportsHealthQuery`: 232 → `subCmd == DAILY &&
  r1Captured229DailyQueryCmd(cmd)` (rename the helper to something
  profile-neutral, e.g. `r1DailyHistoryQueryCmd`).
* `r1ProfileSupportsHealthPageRefresh`, `r1ProfileSupportsSingleFrameDaily`,
  `r1ProfileSupportsDailyPacketAck`: 232 → same as 2.2.9.
* `r1ProfileSupportsActivityReassembly`: add 232.
* Split `r1ProfileSupportsDeviceStatusIngestion` into a battery helper
  (2.2.7, 2.2.9, 232) and a wear-from-deviceStatus helper (2.2.7, 2.2.9
  only). Update the `annotateDeviceStatus` comment.
* Leave PointMeasureQuery, PointIngestion, SleepDataIngestion, LowPower,
  UserInfo and WearStatus unchanged. Optionally, split `LowPower` into read
  and write helpers and admit the read side for 0008 only, where the GET was
  captured.

**`G2_Ring.cpp`**, the sites that compare against 2.2.9 explicitly (named by
function, because another session is editing this file):

* `g2RingTimeSyncTick` and `ringSetupServiceDarkProbe`, the dark-clock
  probes: use DAILY (not POINT) for 232.
* `ringExtractTelemetryCache`, `strictPassive229`: treat 232 the same.
* `ringExtractTelemetryCache`, the deviceStatus/heartbeat branch: gate
  `ringNoteWear` on the new wear helper.
* `ringExtractTelemetryCache`, the HR/HRV/SpO2 branch, **needed before
  admitting 0008**. It sets `endTs = daily.latestTimestamp` for every page.
  On 0008, `latestTs` is the ring-wide latest sample, repeated on every day's
  page, so for an older day `g2HealthApplyTrendDaily` and
  `g2HealthApplyDailyBackfill` would stretch that day's samples up to today.
  Use the page's own end, e.g. last record `bucketEpoch + 3600`, and use
  `latestTimestamp` only when it falls inside
  `[dayStart, dayStart + 86400)`. (Correction: the stored day was affected
  too. It kept the ring-wide latest as an older day's own latest. Fixed in
  section 8c.)

**`G2_Health.cpp`:** `historyLayoutForProfile` → `R1_HISTORY_LAYOUT_DAILY_V1`
for 232.

**Host tests:**

* `r1_protocol_harness.cpp`: replace the 2.3.2 "setup only" block with a
  per-capability table for 0007 and 0008. Keep the negative assertions for
  point/measure, low power, user info, wear and sleep. Add a synthetic
  (non-capture) 0007-shaped HR page whose `latestTs` is in the 1970 range,
  and assert parse OK, `latestTimestampMode == UNKNOWN`,
  `latestTimestamp == 0`. Add a zero-base page and assert
  `dayMode == ZERO_BASE`.
* `ring_daily_ack_harness.cpp`: run the existing 2.2.9 cases for
  `R1_PROFILE_FW_2_3_2_0007` too. Accepted page → one ACK; zero-base or
  other-day page → no ACK; a sleep page → no ACK.
* A reassembly case: a synthetic 92-record activity day split 2/1/0 at the
  observed fragment sizes, parsed with a 232 profile.
* A multi-day case: synthetic HR pages for days D−3 … D, each carrying the
  same day-D `latestTs`. Assert that each page's trend/backfill end stays
  inside its own day.
* `test_r1_protocol.py`'s public-profile mirror check needs no change.

Run: `python3 components/hardwareone/test/host/test_r1_protocol.py
--sanitize` and `python3 components/hardwareone/test/host/test_ring_daily_ack.py`.
Then build the P4 board. A hardware check (Health page refresh on the 0007 and
0008 rings) needs the user.

## 8b. Full 2.3.2 support (second change, 2026-10-18, uncommitted)

After the first hardware check, a poll showed "Refresh incomplete". When the
ring held sleep records, HardwareOne could not parse them, so the sleep step
timed out. The user asked for full support. Added for both 0007 and 0008:

* **Sleep records.**
  * `r1ParseSleepDaily` and `r1ParseReassembledSleepDaily` parse the layout
    in section 3.2. They fail closed on length, type, stage code, an
    inverted or longer-than-a-day period, and stage epochs that don't sum to
    the period.
  * A record made before the ring knew the time parses but is marked
    unanchored and is never stored.
  * `g2HealthApplySleepDaily` files a record under the local day its period
    ends on. The day file gains optional `S,…` lines, up to 8 records per
    day, ascending and unique by start. They are hashed only when present,
    so existing day files load and verify unchanged.
  * A stored record is then acknowledged, using the same history-gated rule
    as the other metrics.
  * Long nights that arrive as multi-part frames go through the existing
    reassembler.
  * The G2 Health menu has a **Sleep** page (time asleep, first start to
    last end, deep/REM/light/awake). `healthstatus` (text and JSON) shows
    the record count and the per-stage times.
* **Complete polls.**
  * A layout-valid sleep record now completes the sleep step.
  * Poll Now runs up to 8 daily rounds. After a round that refused a newer
    day, it waits until that round's day is persisted, then sweeps again,
    so a multi-day backlog drains in one poll.
* **Wear at connect.** After setup, HardwareOne sends one wearStatus GET
  (build 243's `getWearStatus`). The ring's own wear pushes keep it current
  after that.
* **Charge status.**
  * `deviceStatus` byte 1 is kept as charge status (charging, not charging,
    full).
  * The ring's 7-byte status pushes while charging are accepted.
  * The G2 overview shows "chg" or "full" after the battery value.
  * `healthstatus` shows the charge state.

**Host tests.**

* `test_r1_protocol --sanitize`: sleep parser vectors and capability table.
* `test_ring_daily_ack --sanitize`, 113 checks:
  * a stored night gets one ACK;
  * an unanchored night or a newer day's night gets none;
  * the newer day sets the backlog flag;
  * a reassembled night is stored and acknowledged;
  * pushed charging status refreshes battery and charge.
* `test_r1_history_store`, 44 scenarios: sleep round trip, sealed and
  plain, plus the refusal cases.
* `test_health_logging_ui`: row indices after the new Sleep row.
* 19 other host tests that read these files also pass.
* The sleep ACK, backlog flag and charging-push checks were confirmed to
  fail on reverted code.
* The P4 build of `p4-phone-pipe-camera-off-test` is clean.

**Not covered by host tests; check on hardware:**

* the Poll Now backlog rounds;
* the wearStatus reply to the post-setup GET (layout from app code only);
* the Sleep page text;
* a reassembled long night.

The `healthstatus` help line changed, so `docs/COMMAND_REFERENCE.md`
(generated by `tools/command_registry.py`) needs regenerating.

## 8c. Fixes from the adversarial review (2026-10-18)

The parallel session reviewed the first change; its report is
`docs/R1_HEALTH_2_3_2_REVIEW.md`. All seven issues are folded in, and each
new test fails when its fix is reverted.

1. **A sweep closing on a refused activity frame** (high).
   * Problem: the first activity page that parses completed the stage, even
     when the history model refused it. The sweep then closed before the
     key day's page arrived, so that page was never stored or ACKed.
   * Fix: a history-gated sweep now stays open after its last stage until
     the ring has been quiet for 800 ms (4 s cap), so the trailing page is
     still stored and ACKed.
   * Not taken: the review proposed completing the stage only on an
     accepted page. That was set aside because a ring whose only pages are
     refused (0007 pre-sync activity) would then always end partial.
   * Code: `ringHistorySweepSettled` and `sLastHealthPageRxMs`.
2. **Ring-wide latest stored on older days.** `latestInDay` now bounds the
   stored latest in `mergePendingHourlyResult`, `applyCommonToDayLocked`,
   `applyHrvToDayLocked` and `rebuildOneTrendLocked`.
3. **Live backfill from the oldest backlog page.** Only the page whose day
   is not older than the ring's newest sample, or one with no anchored
   latest, seeds the live backfill (`ringPageHoldsNewest`).
4. **Sub-day 1970 latest read as a time of day.** On 2.3.2,
   `classifyDailyTimestamp` no longer reads a sub-day raw value as seconds
   within the day; 2.2.x is unchanged.
5. **Multi-part sleep that fails to parse.** It now records sleep presence
   (`ringRememberSleepCandidateHeader`), so the deadline cannot conclude
   EMPTY. Nothing is ACKed.
6. **byte 1 = 3 ("full").** Already accepted by the second change; a test
   now covers it.
7. **Test gaps.**
   * The dark probe has its own harness (`ring_dark_probe_harness.cpp`, run
     by `test_r1_protocol.py`).
   * New edge cases: exact page ends, HRV pages, multi-record pages, and
     unanchored latest.
   * Telemetry publication is covered for wear and charge.
   * The stored-latest bound has a test in the store harness.

## 8d. advStart with a temple down (2026-10-18, uncommitted, not flashed)

On the P4, ring setup failed `identity-unknown` on every reconnect while the
left temple was not advertising. The ring answered pairAuth, deviceInfo and
systemTime each time. advStart needs both temple addresses, and
`G2_Ring.cpp` read them only from connected temples. The left temple carries
no ring traffic, so its absence locked the ring out for nothing.

`ringResolveAdvIdentity` (G2_Ring.cpp) now takes a connected temple's own
address and falls back to the saved G2 pair (`blePeerSavedTargetSnapshot`,
mac1 left, mac2 right) for a temple that is down. That is what
R1_RING_PROTOCOL.md §5.1 already prescribes ("connected/saved G2 peer
records"). The fallback is used only when both saved addresses parse, are
non-zero, and match every connected temple, so a newly paired pair never
mixes with the old saved half. With no saved address, setup still fails
closed; zeros or a six-byte form are still never sent. When the fallback is
used, an INFO line names the temple without printing an address.

Host test: `ring_adv_identity_harness.cpp`, run by `test_r1_protocol.py`,
splices the resolver and `macParse` and checks the advStart payload on both
2.3.2 profiles. Each of 6 mutations of the resolver fails it.

Hardware check (2026-10-18 14:21, P4, image 5300a5ae plus this change,
sha256 5988b154…):
* With both temples disconnected (`closeg2`), `ringconnect reconnect` logged
  the saved-address line for both temples; advStart was acked and setup
  verified.
* The wearStatus GET sent at connect was answered (possiblyWear).
* Poll Now in the same session: all five DAILY stages verified, the
  deviceStatus read gave charge=notCharging, and the refresh ended ok=1
  partial=0. The ring held one day per metric and no sleep record, so
  backlog rounds and stitching a real night were not exercised.
* On the charger the ring pushed charge=charging about every 30 s and
  notCharging when lifted off.
* Gap: the ring then pushed possiblyWear (3), which was not ingested, so
  Wear stayed "off" while the ring was on the finger. Fixed in section 8e.

## 8e. possiblyWear as its own state (2026-10-18, uncommitted)

Lifted off the charger, the 0008 ring pushes notWear (1) and, seconds later,
possiblyWear (3). It can stay at 3 for minutes before deciding, and it sends 3
on a desk too. HardwareOne used to drop 3, so the last on/off stayed on
screen.

* `G2_Ring.cpp` records wearStatus 0–3 (the push and the connect-time GET
  reply). The 2.2.x deviceStatus wear byte is unchanged.
* `g2RingWearName()` (G2_Ring.h/.cpp) is the one wording: on, off, maybe,
  ? (unknown code), -- (no sample). The G2 overview and info text
  (`wearLabel`), the OLED Health screen, `healthstatus`, the sensor-log text
  line and the web page all use it. The web page reads the new `wearName`
  field in the /api/health/status and `healthstatus json` JSON; the numeric
  `wear` field (CSV and JSON) now can be 3.
* `System_SensorLogging.cpp` now includes G2_Ring.h unconditionally (its
  stubs cover builds without BT/G2). Both the BT-off (xiao-no-bluetooth) and
  BT-on/G2-off (p4-phone-only) configurations compile it, syntax-only.
* Tests in `ring_daily_ack_harness.cpp`: 3 is recorded, 4 is not, 3 is
  published, and the name table plus the G2 label. A source check in
  `test_ring_daily_ack.py` fails if any display file maps wear codes itself.
  Each of 8 mutations fails the suite.
* Hardware check (2026-10-18 15:02, P4 image 480c30b1):
  * On the charger the ring pushed notWear plus charging.
  * Lifted off at 15:02:43, it pushed notWear and notCharging, then
    possiblyWear 0.1 s later. The user saw it read as "maybe".
  * On the finger it pushed wear at 15:07:10, about 4.5 minutes later.
  * `healthstatus json` then showed `"wear":2,"wearName":"on"` and no
    overflow.

## 8f. Automatic battery poll (2026-10-19, uncommitted, not on hardware)

2.3.2 has no point-poll cycle; that cycle carries deviceStatus on 2.2.7.
Without one, only Poll Now asked for battery. Now `ringServiceDeviceStatusPoll`
(G2_Ring.cpp, run from the 500 ms `g2RingTimeSyncTick`) does it:
* It sends the same empty deviceStatus GET as Poll Now, with key 5, as soon
  as setup finishes on a link.
* After that, it asks again whenever 5 minutes pass with no battery sample.
  The ring's charger pushes and Poll Now's read count as samples.
* It never asks while a Poll Now runs. A refused enqueue retries after 30 s.
* The capability is `r1ProfileSupportsDeviceStatusPoll`, 2.3.2 only.
* The reply takes the existing path, so the lens Status corner and the health
  log get battery samples.

Tests: `deviceStatusPollCadence` in `ring_daily_ack_harness.cpp` (202 checks)
and the capability rows in `r1_protocol_harness.cpp`. Each of 9 mutations
fails them. By the user's decision it ships with the phone link v2 image.

Hardware check (2026-10-19, run by the phone-link session; v2 image
032b2b4f, records in tools/p4/companion/private/flash-2026-10-19b/): `ring
health json` showed a valid battery and charge sampled when the ring
connected, with no Poll Now. The P4 was then rolled back to 53c2a9b2, which
does not have the poll.

## 9. Open questions and follow-ups

* deviceStatus byte 1 on 2.2.7/2.2.9: it is charge status on 2.3.2.0008. If
  the same holds on the older firmware, HardwareOne's wear indicator on those
  profiles is really "not charging". That is a separate fix. 2.2.7 and 2.2.9
  were left unchanged.
* Battery while charging on 2.3.2: accept the ring's 7-byte status pushes and
  byte 1 = 3 ("full") in `ringDeviceStatusPayloadValid`.
* The trailer's meaning and the right way to date 1970-range sleep records.
  Needed before sleep ingestion.
* Whether withholding the userInfo answer affects ring algorithms.
