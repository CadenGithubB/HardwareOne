# On-device settings backlog — G2 glasses & R1 ring

Goal: change the settings that normally require the Even Realities phone app, from our own
firmware. Two surveys (2026-07-30), 32 agents, 63 ring candidates + full G2 settings-service
decode, each top candidate adversarially re-verified against the actual files.

## Scope rule

**Every item below is a replay of traffic a shipping client demonstrably sends.** An item
qualifies only if FlutterApp or the rev-share python demo has a real setter with a real caller
that we can copy. Schema-only knobs, opcodes no client ever writes, destructive or
unverifiable commands, and OTA are all out of scope and are not listed. This is a deliberate
narrowing — see "What this rule excluded" at the end for what it cost.

> ## ✅ HW PROBE RESULT 2026-07-30 — the G2 write path WORKS
>
> Firmware under test: **2.2.4.34** (both temples). Our protos are `v2.1.0_beta` and the RE
> reference targets 2.2.0.0011 — we are driving a newer firmware than any source we hold, and
> the field numbers still match.
>
> ### Writes land, and they self-verify
>
> ```
> 21:23:20  f2 = 35                                   (drifting; auto-brightness is on)
> 21:23:46  TX  g2probe 09 1 1A040A021032             (brightness = 50)
> 21:23:46  RX  flag=0x00  08 01 | 10 FA 01 | 1A 04 0A 02 10 32
> 21:23:49  f2 = 50    21:23:51  f2 = 50    21:24:07  f2 = 50
> ```
>
> Wear detection likewise: `f10` **absent** in the 21:20:57 read, present as `1` in every frame
> from 21:22:58 on, right after its write was acked.
>
> **The ack is the verification primitive — better than polling the echo field.** A successful
> write is mirrored back at `flag=0x00` as
> `08 01 | 10 <ourMagic> | <the exact f3 body we sent>`. Immediate and magic-correlated.
>
> **A no-op write appears not to be acked.** `autoAdjust=1` when `f18` was already `1` produced
> no response. Treat "no ack" as ambiguous (no-op *or* rejected), not as failure.
>
> ### Per-field presence is preserved
>
> The ack echoed `0A 02 10 32` — *only* the field we set, not a fully-populated
> `DeviceReceive_Brightness`. The device is not defaulting the sibling fields. That is the
> mechanism that would have zeroed lens calibration, so the accidental-overwrite risk is much
> lower than assumed. Inference from one echo, not proof.
>
> ### An earlier cold session did NOT work — unexplained
>
> The same three writes at 21:16 got no ack and changed nothing. The only visible difference is
> that `g2battery` ran in between — a `settingInfoType=1` (`APP_REQUIRE_BASIC_SETTING`) read,
> where the manual probes had only ever sent `settingInfoType=0`. From that point on every
> response carries `field 1 varint=1`, absent in all earlier frames. **Correlation, not proven
> cause.** Cheap test: power-cycle, write cold, then `g2battery` and retry. If it reproduces,
> a BASIC_SETTING read is a required priming step and belongs in the plumbing.
>
> ### CONFIRMED BUG — the write-ack corrupts our parser state
>
> ```
> [G2-R] Firmware version: '' (was '2.2.4.34')
> [g2-status-TX] {"s":"connected",…,"w":"fw-ver"}
> ```
>
> `g2ParseSettingVersion` walked **outer f3** (`deviceReceiveInfoFromApp` — our own echoed
> write) as if it were outer f4, decoded the wear-detection sub-message as a version string,
> wiped the cached firmware version, and fired a spurious status broadcast. This is rule 3 below,
> demonstrated on hardware. **Fix it as part of item 4, not after** — every write we send will
> trip it.
>
> ### Live field map off 2.2.4.34
>
> | Field | Observed | Meaning |
> |---|---|---|
> | f1 | 1 | settingInfoType |
> | f2 | 47→35→**50** | brightness — drifts on its own while auto is on |
> | f3 | 8 | Y coordinate |
> | f5 / f6 | `"2.2.4.34"` | left / right software version |
> | f7 | 1 | head-up switch **on** |
> | f8 | 40 | head-up angle |
> | f10 | **1** | wear detection — appeared after our write |
> | f11 | 1 | deviceRunningStatus (intermittent) |
> | f12 | 68 | battery |
> | f18 | 1 | auto-brightness **on** |
>
> **Never reported: f14 (silent mode), f15/f16 (lens calibration), f13, f17, f19.**
> Consequences: item 7 (silent mode) has **no read-back on this firmware** — its only
> verification is the write-ack. And rule 1 below ("snapshot calibration first") **cannot be
> executed as written**, because f15/f16 are not readable; the per-field-presence finding above
> is what de-risks it instead.
>
> ### Open question
>
> `f18=1` means auto-brightness is on, and `f2` drifts by itself (47→35). Our manual write held
> at exactly 50 for ~21 s across four pushes. Whether the ALS eventually overrides it is
> **untested** — needs a longer observation before we promise users a stable manual level.

Verdict:

- **G2 — HW-VALIDATED.** The whole settings surface is ONE protobuf message on a service we
  already poll safely. Reads, writes and write-acks are all confirmed on firmware 2.2.4.34.
  Items 1 and 3 are proven end-to-end; the rest share their exact mechanism.
- **R1 — mostly not achievable.** Cadence, goals, wear-hand, DND, LED/vibration, units: no
  opcode exists in any of the four sources. The ring list is therefore mostly plumbing and one
  live security fix, not settings.

Evidence tags: `HW` acked on our hardware · `CAP` seen in a BLE capture · `APP` a shipping
client actually sends it.

Path aliases: `RS/` = `docs/evenrealities_rev_share-main/proto/proto_out_v2.1.0_beta_v3/protos/g2/`
· `FA/` = `docs/FlutterApp-main/lib/src/` · firmware paths under `components/hardwareone/`.

---

## G2 glasses — 8 items

All sid=0x09, `flag=0x20`, **right temple only**, `commandId=1` (`DeviceReceiveInfo`), outer
field 3. We have never sent `commandId=1`; we already send `commandId=2` (that is how
`g2battery` works), so transport, envelope and RX parse are proven.

`cmd_g2probe` blocks **only** `sid == 0x80` (`G2_Glasses.cpp:14754`) — sid 0x09 is open, so
every item has a free probe.

| # | Item | Payload after `08 01 10 <magic>` | Read-back echo | Effort | Evidence |
|---|---|---|---|---|---|
| 1 | **Manual brightness** 0-100 | `1A 04 0A 02 10 <v>` | f4→f2 `autoBrightnessLevel` (misnamed; it is the manual level) | XS | `APP` `FA/services/g2_settings_service.dart:126` + `CAP` value 25 in 58 replies across all four `docs/even-g2-protocol-main/captures/*.log` |
| 2 | **Auto-brightness (ALS) on/off** | `1A 04 0A 02 08 <0\|1>` | f4→f18 `autoBrightnessSwitchRestored` | XS | `APP` `:137` |
| 3 | **Wear detection on/off** | `1A 04 2A 02 08 <0\|1>` | f4→f10 `wearDetectionSwitchRestored` | XS | `APP` `:149` + `CAP` `50 01` in ~26 replies in `captures/auth-sequence.log` |
| 4 | **Shared plumbing** (enabler for all others) | `g2BuildSettingInfoWrite()` + `g2ParseSettingEcho()` | — | S | see below |
| 5 | **Display vertical position (Y)** | `1A 04 12 02 08 <lvl>` | f4→f3 `yCoordinateLevelRestored` | XS | `APP` `:171` + `CAP` two live values (6 in `fresh-pairing.log`, 12 in `teleprompter-session.log`) |
| 6 | **Display horizontal/depth (X)** | `1A 04 1A 02 08 <lvl>` | f4→f4 `xCoordinateLevelRestored` | XS | `APP` `:160` + `CAP` value 1 |
| 7 | **Silent mode / DND** | `1A 04 32 02 08 <0\|1>` | f4→f14 + we already decode the unsolicited push (`System_G2_Protocol.cpp:1560`) | S | `APP` `:182` + working python demo `RS/../demos/pythonapp/service_ui_settings.py:10` |
| 8 | **Units / time / date / temperature format** | `1A 04 4A 02 <f> <v>`, f3→f9 `APP_Send_Universe_Setting` f1-f5 (`RS/g2_setting.proto:51-57`) | **none — no `*Restored` for any of the five** | M | `APP` — the python demo sends it on connect init |

### Zero-code probe sequence

```
g2settings verbose on            # dump every inner field of sid=0x09 pushes (G2_Glasses.cpp:7368)
g2battery                        # baseline for f2/f10/f15/f16/f18
g2probe 09 2 22020800            # brightness-info read (settingInfoType=0) — never sent before
g2probe 09 1 1A040A021032        # brightness = 50
g2probe 09 1 1A040A020801        # auto-brightness on   (…0800 = off)
g2probe 09 1 1A042A020801        # wear detection on    (…0800 = off)
g2battery                        # confirm the *Restored field moved
```

`g2probe` hardcodes `magicRandom=250` and picks the right arm via `pickEvenAIArm`
(`G2_Glasses.cpp:14473`), matching FlutterApp's `sendProtoToRight`. The
`settingInfoType=0` read is itself an `APP` item — FlutterApp has both read variants
(`FA/services/g2_settings_service.dart:100`, `:113`) and we only ever send `=1`.

### Three rules that are not optional

1. **Snapshot lens calibration before the first brightness write.** `leftCalibration` /
   `rightCalibration` are fields 3 and 4 of the **same** `DeviceReceive_Brightness` message as
   brightness (`RS/g2_setting.proto:128-129`). The reference app writes exactly one field, but
   if the firmware does not track per-field presence a one-field write delivers
   `calibration=0`. Read f15/f16 first, write, re-read. A dark lens is recoverable; zeroed
   optical calibration is the only thing here you cannot guess your way out of. Never write
   f3/f4 yourself — they are factory/optician alignment values, not preferences.
2. **Right temple only.** Our existing sid=0x09 sender `cmd_g2battery` sprays *both*
   (`G2_Glasses.cpp:15644`). Do not copy it.
3. **Scope every new parser to outer f4.** `deviceSendInfoToApp.f2` is silent-mode while
   `deviceReceiveRequestFromApp.f2` is brightness — same field number, unrelated meaning. This
   already bit us; see `System_G2_Protocol.cpp:1556-1559`.

### Caveat on item 7 (silent mode)

The reference app tears down the EvenHub plugin — sid=0xE0, i.e. **our** render surface
(`System_G2_Protocol.h:130`) — the moment silent engages
(`FA/background/ble_background_service.dart:1820-1848`), and skips every dashboard push. It
also suppresses the shipped `NSINK_G2` native-notification cards. Gate admin-only, emit a
`cliHint`, and do not wire it to an automation action until HW-tested with a hijack page live.

### Caveat on items 5 and 6 (display position)

These move **our own hijack pages**, not just native UI. Read the current value first and step
relatively; never jump to an absolute you guessed. Range is unknown.

### The plumbing (item 4)

```c
// System_G2_Protocol.cpp — directly beneath g2BuildSettingBasicRequest (:1497).
// Same shape, one extra nesting level, commandId=1 instead of 2.
size_t g2BuildSettingInfoWrite(uint8_t seq, uint32_t magic,
                               uint8_t innerField,   // 1=bright 2=Y 3=X 5=wear 6=silent 9=units
                               uint8_t leafField,
                               uint32_t value,
                               uint8_t* out, size_t outCap);
```

Every primitive already exists: `g2PbWriteUint32` / `g2PbBeginNested` / `g2PbEndNested`
(`System_G2_Protocol.h:249-256`), `g2BuildEnvelope`, `allocSeq()`, `sendEnvelope`. No transport
work at all. After this, each setting is ~5 lines.

Bake into the helper, not the callers:

- **Hard allowlist `innerField ∈ {1,2,3,5,6,9}`.** Reject everything else. Neighbouring field
  numbers in this message reach knobs that are out of scope for this backlog, and a single
  off-by-one is all it takes to hit one. An allowlist, never a range check.
- Clamp `value` — a negative int32 is a 10-byte sign-extended varint and blows the 32-byte
  payload assumption.
- New magic `G2_MAGIC_SETTINGS_WRITE 209` (208 is `G2_MAGIC_SETTINGS`, 210+ is the image base;
  must stay ≤255, firmware compares only the low byte).

Twin: **`g2ParseSettingEcho()`**, scoped to outer f4, pulling the whole
`DeviceReceiveRequestFromAPP` block (f2 brightness, f3 Y, f4 X, f10 wear, f14 silent, f15/f16
calibration, f18 auto-brightness) into a cached struct, fired from the existing sid=0x09 RX
handler (`G2_Glasses.cpp:7299`). That is the verify-after-write loop and it gives every UI
surface real values instead of guesses.

**Pre-req:** the existing sid=0x09 parsers have a CONFIRMED unbounded-varint OOB read reachable
from a spoofed BLE peer (`docs/AUTH_SECURITY_REVIEW.md:819-821`; defects at
`System_G2_Protocol.cpp:1513`, `:1554`, `:1334`, `:1632`). The write builder inherits none of
it, but `g2ParseSettingEcho` would be a fourth instance. ~3-line shared bounds guard.

**Surfacing on all three surfaces cheaply:** one real CLI command (`g2glassesset <knob> [value]`),
then have the lens page and OLED row call it as a string — precedent at
`G2_Page_CameraSettings.cpp:210` → `g2SubmitHijackCommand`, and `executeOLEDCommand` in
`OLED_Mode_Bluetooth.cpp`. Do **not** extend `G2_Page_Settings.cpp`; that is our own registry
editor and mixing them muddies the shipped interactive-settings feature.

**Footgun:** if a lens tap triggers the write it must go through the worker/enqueue pattern.
Multi-step BLE pipelines run inline on the notify-task tap dispatcher deadlock the write mutex
permanently — "only a reboot recovers" (`docs/G2_PROTOCOL.md:1774-1783`).

### G2 — already covered, do not re-request

| Setting | Where |
|---|---|
| Wall clock + timezone | `g2BuildDevCfgTimeSync` (`System_G2_Protocol.cpp:1778`), auto-pushed per connect (`G2_Glasses.cpp:8415`), CLI `g2devcfg time` |
| Native notification master enable + auto-display | `g2BuildNotifCtrlEnable` (`:1848`), auto-primed per connect (`G2_Glasses.cpp:8487`) |
| Notification per-app whitelist off | `g2BuildNotifWhitelistDisable`, same prime path |
| Basic-settings **read** | `g2BuildSettingBasicRequest` (`:1497`), only caller `cmd_g2battery` |

### G2 — does it survive the phone app reconnecting?

Not observed either way; any confident claim is unsupported. Pointing our way: every readable
setting is named `*Restored`, and the reference app has **no on-connect settings push** — its
only outbound calls are two reads plus six user-triggered setters. Pointing against: the
notification whitelist and the dashboard+menu config **are** re-pushed on every connect.

The design that makes it moot: copy what we already do for time sync and notification priming —
keep a desired-state table on the ESP32, re-assert on connect, use the `*Restored` echo to
detect drift. Works for items 1-3 and 5-7. Does **not** work for item 8 (no read-back).

Also unproven: persistence across a glasses power-cycle. One power-cycle + `g2battery` during
the probe sequence settles it for all five read-backed settings at once.

---

## R1 ring — capture-backed implementation status (2026-08-03)

The August official-app captures supersede the earlier speculative backlog. The supported
production profile is deliberately narrow: exact R1 firmware `2.2.7.0005`, otherwise Unknown
and fail closed for writes/history decoding.

| Item | Evidence-backed meaning | Implementation rule |
|---|---|---|
| Serialized command owner | Multiple old producers raced the one encoder and treated queueing as success | One fixed-capacity owner assigns serials and reports queued/written/acked/verified separately |
| `packetAck` `0x7E` | Official app ACKs every valid daily data notify with its module/cmd/subCmd/data serial | Priority lane; never ACK invalid CRC/length or empty command ACKs |
| `healthSettings` `0x0E` | SET byte 4 is health collection; ring returns only an empty ACK | Persist desired Preserve/Off/On; observed stays Unknown because no GET/readback is proven; default Preserve |
| `systemSettings` `0x0F` | Byte 5 is low-power; SET is epoch + switch type 0 + enabled + six zero bytes | Same desired/observed model; do not confuse it with ring hand |
| `advStart` `0x0A` | 12 bytes: reversed right-temple MAC followed by reversed left-temple MAC | Require both authoritative temple identities; no six-byte/zero/captured fallback |
| Daily history | HR/SpO2 common hourly records, 16-bit HRV records, seven-byte activity buckets | Decode only after exact firmware identification and strict CRC/length/range validation; preserve the varying ring-owned trailer as opaque metadata |
| Raw sender | Can reach destructive/unverified SET opcodes | Admin-gated; dangerous system SET additionally requires explicit confirmation |

### Explicitly excluded: userInfo/profile writes

The official app's 12-byte personal profile frame is now understood only well enough to
recognize as sensitive calibration input. It has no safe read-back, and earlier experiments
showed bad values being replayed on reconnect. HardwareOne may decode it for private research,
but production code has no builder, command, setting, or UI for it. It must never calculate an
age from a birthday or invent a value for the firmware.

### Link and persistence policy

Settings require HardwareOne's direct R1 BLE link. The default desired value is Preserve, so
first connect performs reads but sends no privacy/power SET. Once the user explicitly chooses
On or Off, HardwareOne may reassert that desired state after reconnect, then updates observed
state only from ACK/readback. The official app may later change the ring; Preserve accepts that
new observed state instead of fighting it.

The implementation remains independent of G2 dominant-hand and display-off long-press. Hand is
a G2 field; changing it may trigger a routing refresh using the same dual-MAC `advStart`, but
there is no R1 hand bit.

---

## What this rule excluded

Recorded so the same ground is not re-surveyed later.

| Dropped | Why it failed the scope rule |
|---|---|
| G2 head-up (tilt-to-wake) switch + angle | The glasses plainly implement it — captures show live angle values 30 and 20 — but **no FlutterApp setter and no demo caller exists**. We would be the first client to write it |
| G2 UI language (sid 0x20) | Decode-only in FlutterApp (`FA/protocol/g2_proto_decoder.dart:70-71`); nobody anywhere sends it. Write-only, no read-back, language index values unknown |
| G2 dashboard auto-close (sid 0x20) | Same service, same problem — no client sends it. Was only proposed as recon to prove sid 0x20 exists |
| G2 dashboard display config (sid 0x01) | Collides with our own `G2_SID_APP_LAUNCH` (`System_G2_Protocol.h:86`) and duplicates concepts from item 8 |
| R1 `reportEnable` re-probe | `healthSettingReportEnable` is defined at `r1_messages.dart:351` but its **only caller is a unit test** — verified by grep over the whole app. No shipping client sends it. This was the highest-upside free experiment in the original survey; it is excluded on principle, not on evidence, and the finding that our own `cmd=0x07` is probably mis-addressed (newer map has `0x07=sportRunCtrl`, `healthSetting=0x09`) still stands and is still worth knowing |
| Additional R1 settings outside captured `0x0E` health collection and `0x0F` low power | No official-app toggle capture proves another safe setting; do not infer one from enum names |
| Destructive / unverifiable opcodes on both devices | Out of scope by definition. Not enumerated here; item 1 on the ring list guards the raw path that could otherwise reach them |
| OTA on both devices | Out of scope by explicit instruction |

## What is actually missing

The captured R1 settings are now resolved. The most useful remaining capture is a late-day,
high-activity official-app history fetch to determine whether a large activity day is sparse,
paged at the R1 protocol layer, or fragmented at BLE. Any additional cadence/goal/DND setting
still requires a dedicated official-app toggle capture; enum names alone remain insufficient.
