# Even Realities R1 Ring — Protocol Reverse-Engineering Notes

## Contents

**Foundations** — what the device is and how the wire format works
- [Confidence legend](#confidence-legend) — tag system used throughout
- [§1 Hardware identification](#1-hardware-identification) — BLE name, address type, GATT UUIDs, MTU, single-central caveat
- [§2 Wire envelope format](#2-wire-envelope-format-) — frame layout, CRC32/CRC16, the firmware's intentional CRC16 quirk, worked example
- [§3 Status byte](#3-status-byte-model5-) — type/method/ack bit packing
- [§4 Module / cmd / subCmd reference](#4-module--cmd--subcmd-reference) — top-level opcode taxonomy

**Connecting** — the entry handshake every session needs
- [§5 Authentication & setup sequence](#5-authentication--setup-sequence-) — pairAuth → systemTime → advStart, with the MAC-order caveat

**Opcode catalogue** — every opcode we touched, with confidence tags
- [§6 system module opcodes](#6-system-module-opcodes--detailed) — deviceStatus, deviceInfo, wearStatus, advStart, algoKey, healthSettings, deviceSn, nvRecover, heartbeat, …
- [§7 health module opcodes](#7-health-module-opcodes--detailed) — point / daily / measure for HR, HRV, SpO2, temperature, activity, sleep
- [§8 sport module](#8-sport-module-x) — silent on this firmware
- [§9 testable module](#9-testable-module-x) — silent / untested

**Behaviour** — runtime characteristics worth knowing
- [§10 Caching behaviour](#10-caching-behaviour-) — why polling faster than 30 s is pointless
- [§11 nvRecover and the algoKey](#11-nvrecover-and-the-algokey--with-implications) — per-device identifiers and what they imply for the bridge
- [§12 R1 ↔ Host runtime status](#12-r1--host-runtime-status) — at-a-glance "can I do X" matrix

**The bridge attempt** — driving the official R1↔glasses pairing from a third-party host
- [§13 The R1 ↔ G2 bridge attempt — what we learned the hard way](#13-the-r1--g2-bridge-attempt--what-we-learned-the-hard-way) — phase-by-phase results, both approaches tried, why we believe it's currently blocked, and the practical workaround
- [§14 Open questions](#14-open-questions) — seven unsolved problems for anyone continuing

**Reference material**
- [§15 Example: full polling session (annotated)](#15-example-full-polling-session-annotated) — connect → query → decode walkthrough with real wire bytes
- [§16 Appendix A: opcode quick-reference](#16-appendix-a-opcode-quick-reference) — full system + health opcode tables
- [§17 Appendix B: status-byte values seen on the wire](#17-appendix-b-status-byte-values-seen-on-the-wire)
- [§18 Appendix C: bridge connRet codes](#18-appendix-c-bridge-connret-codes-sid0x80-cmd6-from-temple)
- [§19 References](#19-references) — community RE projects this doc draws on, with their own caveats
- [§20 Contributing back](#20-contributing-back) — how to update this doc if you verify or refute claims

---

## About this document

A consolidated, share-friendly account of what is currently known about the
Even Realities R1 ring's BLE protocol, derived from a mixture of:

- **Empirical testing** against firmware **2.2.0.0011** with hardware
  reverse-engineering and live wire captures.
- **Code reading** of the community
  [`FlutterApp-main`](https://github.com/) reference implementation (the
  third-party Flutter port of the official Even app).
- **Code reading** of the community `evenrealities_rev_share` Python
  codec/parser.
- A handful of inferences clearly marked as such.

The official manufacturer (Even Realities) has not published a protocol
specification. Everything here is **reverse-engineered**. Treat it as a
working understanding, not authoritative documentation.

> **Scope:** this document covers the R1 ring's wire protocol and what we
> learned attempting to drive the R1↔G2-glasses bridge from a third-party
> host. It does NOT cover the G2 glasses' own protocol except where the
> bridge handshake intersects.

> **Placeholders:** every MAC address, device serial, and per-device key
> in this document uses **placeholder values** (e.g. ring MAC
> `f8:29:ca:11:22:33`, ring name `EVEN R1_112233`, right-temple MAC
> `c8:8d:65:44:55:66`, deviceSn `XXXXXXXXXXXXXXX`, algoKey
> `ca112233xxxxxxxxxxxxxxxx`). The OUI prefix bytes (`f8:29:ca` for the
> ring, `c8:8d:65` for G2 right temples) are preserved because they're
> the same across all units of each model. Substitute your own values
> when working with real hardware.

---

## Confidence legend

Throughout the document, every claim is tagged:

| Tag | Meaning |
|----|---------|
| ✓ **Verified** | Empirically tested against live R1 hardware on firmware 2.2.0.0011. Behaviour reproducible across multiple sessions. |
| ◐ **Partial** | Works, but with caveats noted inline (firmware quirk, intermittent, only some payloads, etc.). |
| ◯ **Inferred** | Believed correct from code inspection of the FlutterApp/Python references, but we did not directly verify on hardware. |
| ⚠ **Speculative** | Best-guess interpretation of observed bytes. Could easily be wrong. |
| ✗ **Tried, no response** | We sent it, the ring stayed silent. Could mean unsupported, mis-formatted, or requires prior state we don't have. |
| 📖 **Claimed elsewhere, unverified** | Documented in a reference (FlutterApp comment, Python codec, community doc) but we have no first-hand evidence for it. |

If you adopt this document, please update tags as you verify or invalidate
claims against your own hardware. Note the firmware revision you tested
against — there is evidence of behaviour drift between versions (the
Python codec README explicitly says "R1 parsing is currently semi broken
- it seems like opcode/command mapping is incorrect" for the firmware
they were targeting).

---

## 1. Hardware identification

### 1.1 BLE advertising name ✓
Pattern: `EVEN R1_XXXXXX` where `XXXXXX` is an uppercase 6-hex-digit
suffix (the last 3 bytes of the device's BD_ADDR, displayed without
separators).

Example: `EVEN R1_112233` at MAC `f8:29:ca:11:22:33` (placeholder values; substitute your unit's last 3 MAC bytes).

### 1.2 BLE address type ✓
**Random Static** (Bluetooth Spec: top two bits of the MSB are `0b11`,
i.e. first byte ≥ `0xC0`). When initiating a directed connect by MAC,
you must specify `BLE_ADDR_TYPE_RANDOM` rather than the usual `PUBLIC`,
or the controller will time out at ~30 seconds.

### 1.3 GATT layout ✓
| UUID | Role |
|------|------|
| `bae80001-4f05-4503-8e65-3af1f7329d1f` | Primary service |
| `bae80011-4f05-4503-8e65-3af1f7329d1f` | Write characteristic (write-without-response) |
| `bae80013-4f05-4503-8e65-3af1f7329d1f` | Notify characteristic |

The notify char does not advertise `canWriteNoResponse`, only `canNotify`.
Subscribe to it normally; all R1 responses arrive as notifies on this
characteristic.

### 1.4 Default MTU ✓
Negotiates to 64 bytes if requested. The ring will accept lower MTUs
without issue, but the largest captured payload (`nvRecover`) is ~80 B,
so you'll fragment some responses if you stay at the 23-byte default.

### 1.5 Single-central peripheral ◐
Empirically the R1 will **refuse a second simultaneous central connection**
in this firmware version. While we hold the ring's BLE link, attempts by
another central (e.g. a paired G2 right temple) to connect time out. This
contradicts the FlutterApp's apparent assumption of multi-central; see
§13 for the bridge-attempt analysis.

---

## 2. Wire envelope format ✓

Every BLE write to (and notify from) the ring is one **logical R1 frame**
in the following layout:

```
+------+----------------+--------------------------------------+
| [0]  | transferType   | always 0x00 in our usage             |
| [1..4] CRC32          | model bytes, Castagnoli polynomial   |
| [5..N+5] model        | 12-byte header + payload             |
+------+----------------+--------------------------------------+
```

### 2.1 Model header (12 bytes) ✓

| Offset | Field | Value |
|--------|-------|-------|
| 0 | version | `0x64` (always) |
| 1 | module | see §4.1 |
| 2 | moduleVersion | `0x64` (always) |
| 3 | serial low byte (LE) | per-session counter, starts at 1 |
| 4 | serial high byte (LE) | excluded from CRC16 |
| 5 | status byte | see §3 |
| 6 | cmd | see §4.2 |
| 7 | subCmd | see §4.3 |
| 8 | modelLength low (LE) | = 12 + payloadLen |
| 9 | modelLength high (LE) | |
| 10 | CRC16 low (LE) | excluded from CRC16 |
| 11 | CRC16 high (LE) | excluded from CRC16 |
| 12.. | payload | up to ~70 B in observed traffic |

### 2.2 CRC algorithms ✓

**CRC32 (over the model bytes):**
- Polynomial: `0x1EDC6F41` (Castagnoli, NOT zlib's `0x04C11DB7`)
- Init: `0`
- No reflection, no final XOR
- Self-test against captured FlutterApp fixtures passes byte-for-byte

**CRC16 (in the model header):**
- CCITT-XMODEM-like, init `0xFFFF`
- Input excludes bytes `[4]` (serial high), `[10]` (CRC16 low), `[11]` (CRC16 high)
- I.e. input = `model[0..3] ++ model[5..9] ++ model[12..]`

### 2.3 Firmware quirk: outbound CRC16 is wrong ◐

**The R1 ring intentionally emits a wrong CRC16 on every frame it
sends to the host.** Verified empirically and confirmed by the
FlutterApp's test fixtures, which mark every captured ring→phone
packet with `crc16Ok=false`. Don't reject inbound frames on CRC16
mismatch — only validate CRC32. Our decoder reports `crc16Valid` and
`crc32Valid` as separate booleans for this reason.

The CRC32 we generate **must** be correct on outbound frames; bogus
CRC32 will cause the ring to silently drop the frame.

### 2.4 Worked example: pairAuth frame ✓

Bytes sent (verified against FlutterApp test fixture, serial=1):

```
00 A2 5A AC 92 64 01 64 01 00 03 00 08 0D 00 A7 64 01
```

Decomposed:

| Bytes | Field | Value |
|-------|-------|-------|
| `00` | transferType | 0 |
| `A2 5A AC 92` | CRC32 | of model bytes that follow |
| `64` | model[0] version | 0x64 |
| `01` | model[1] module | 1 = system |
| `64` | model[2] moduleVersion | 0x64 |
| `01 00` | model[3..4] serial | 1 (LE) |
| `03` | model[5] status | notify=0, get=0, ok=0 → 0x00... wait, actually 0x03 here is notify/SET/ok — different from what the FlutterApp's "pairAuth" docstring claims; see §6 |
| `00` | model[6] cmd | 0 = system |
| `08` | model[7] subCmd | 8 = pairAuth |
| `0D 00` | model[8..9] modelLength | 13 (12 + 1 payload byte) |
| `00 A7 64` | model[10..11] CRC16 + start of payload | (the doc-conflict you'll find chasing this is real — see §3) |
| `01` | payload | the literal byte 0x01 |

Note: there's a status-byte interpretation conflict between the FlutterApp's
docstring (claims `notify/get/ok`) and the actual fixture bytes
(`0x03` = `notify/SET/ok` per the bit layout in §3). On firmware
2.2.0.0011 we send `0x03` and it works. Trust the fixture, not the
docstring.

---

## 3. Status byte (model[5]) ✓

A single byte packing three fields:

| Bits | Field | Values |
|------|-------|--------|
| bit 0 | type | 0=notify, 1=ack |
| bit 1 | method | 0=get, 1=set |
| bits 2-3 | ack | 0=ok, 1=error, 2=refuse, 3=notSupport |

Common combinations seen on the wire:

| Hex | Decoded | Used for |
|-----|---------|----------|
| `0x00` | notify/get/ok | Most query frames we send |
| `0x02` | notify/SET/ok | systemTime, advStart in some captures |
| `0x03` | ack/SET/ok | Very common ring response status |
| `0x01` | ack/get/ok | Some ring responses |

When the ring rejects a request, expect bits 2-3 to be set:

| Hex | Decoded |
|-----|---------|
| `0x05` | ack/SET/error |
| `0x09` | ack/SET/refuse |
| `0x0D` | ack/SET/notSupport |

`refuse` typically means "valid request but not allowed in current
state." `notSupport` means "this opcode doesn't exist in this firmware."

---

## 4. Module / cmd / subCmd reference

### 4.1 Modules (model[1]) ✓

| Hex | Name | Status |
|-----|------|--------|
| `0x01` | system | ✓ Many opcodes verified |
| `0x02` | health | ✓ Several opcodes verified |
| `0x03` | sport | ✗ All cmds tried produced no response |
| `0x7F` | testable | ✗ Untested / appears reserved |

### 4.2 system module cmds ✓

The system module uses `cmd=0` for all standard messages. Differentiation
is by subCmd (see §6).

### 4.3 health module cmds ✓

| Hex | Name |
|-----|------|
| `0x01` | heartRate |
| `0x02` | spo2 |
| `0x03` | temperature |
| `0x04` | hrv |
| `0x05` | activity |
| `0x06` | sleep |
| `0x07` | healthSet |

### 4.4 health module subCmds ✓

The same three subCmds work across heartRate / hrv / spo2 / temperature.
activity supports `daily` only; sleep semi-untested.

| Hex | Name | Meaning |
|-----|------|---------|
| `0x01` | daily | Aggregated daily history (multiple records per response) |
| `0x02` | point | Most recent measurement point (single sample) |
| `0x03` | measure | Start a real-time sampling session |

`measure` may take several seconds to start emitting; the ring needs to
boot the PPG algorithm. Some (cmd, subCmd) pairs return
`status=ack/refuse` — the ring rejects unsupported combinations cleanly,
so feel free to probe.

---

## 5. Authentication & setup sequence ✓

This is the entry handshake every R1 session starts with. **No
server-issued key is required** — the literal byte `0x01` is the
entire auth payload. (Earlier community RE attempts speculated about
a "pkey" or remote-server token. Empirically wrong.)

The sequence is three messages, each waited for by ack:

```
Step 1: pairAuth
  module=0x01 (system)
  cmd=0x00 (system)
  subCmd=0x08 (pairAuth)
  status=0x03 (notify/SET/ok)        — see §3 caveat
  payload=[0x01]                      — the literal byte 1, that's it
  Wait: ack → 1 second delay

Step 2: systemTime
  module=0x01, cmd=0x00, subCmd=0x05
  status=0x02 (notify/SET/ok)
  payload = i16(tzMinutes_LE) + u32(epochSeconds_LE)
  Wait: ack → 200 ms delay

Step 3: advStart
  module=0x01, cmd=0x00, subCmd=0x0A
  status=0x00 (notify/get/ok)
  payload = 6-byte G2-right-temple BLE MAC, IN ORDER (NOT reversed)
  Wait: ack → 200 ms delay
```

After step 1, the ring acks then begins emitting telemetry on the
notify char if the user is wearing it.

### 5.1 Important MAC-order caveat ✓

`advStart` payload is the temple's MAC in **BLE address order** (i.e.
the bytes as they appear in the human-readable `aa:bb:cc:dd:ee:ff`
form, MSB-first). It is **NOT reversed**.

This is contrary to:
- An older comment in the FlutterApp `RingPacketEncoder` source
  that uses the parameter name `macReversed` (the parameter name is
  misleading; actual call sites pass MSB-first bytes)
- An earlier version of our own implementation, which reversed the
  bytes and broke the bridge handshake for several days

By contrast, the **`RING_CONNECT_INFO` G2 frame** (sent to the temple,
not to the ring) DOES reverse the ring's MAC. That inconsistency is
real — we verified both directions against the FlutterApp source.

---

## 6. system module opcodes — detailed

Each subsection below documents a single subCmd. Where we have a real
captured payload, it's shown verbatim with annotations.

### 6.1 deviceStatus (subCmd=0x01) ✓

Send: empty payload, `notify/get/ok`.

Response payload: 7 bytes.

```
Example: [41 02 01 00 00 00 00]
```

| Offset | Bytes | Meaning |
|--------|-------|---------|
| 0 | 1 | ⚠ Probable battery percent (`0x41` = 65 in decimal). Stable within a session, drifts down across days. The single strongest piece of evidence: across 9 queries spanning ~4 minutes, byte 0 was constant at `0x46`. Across days it falls in single-digit increments. Strongly looks like a charge gauge. |
| 1 | 1 | wearStatus (0=unknown, 1=notWear, 2=wear). Matches `wearStatus` opcode payload exactly. |
| 2 | 1 | ⚠ Always `0x01` in observed traffic. Maybe a "valid" / "ready" flag. |
| 3..6 | 4 | Always all zeros in observed traffic. Reserved? |

### 6.2 deviceInfo (subCmd=0x02) ✓

Send: empty payload.

Response payload: 32 bytes. Two 16-byte ASCII strings, null-padded.

```
Example: [32 2E 32 2E 30 2E 30 30 31 31 00 00 00 00 00 00
          36 30 33 4D 56 31 2E 39 2E 33 00 00 00 00 00 00]
```

Decoded:
- First 16 bytes: firmware version = `"2.2.0.0011"`
- Last 16 bytes: hardware version = `"603MV1.9.3"`

Trim each half at the first non-printable byte for safe display.

### 6.3 wearStatus (subCmd=0x03) ✓

Send: empty payload.

Response payload: 1 byte.

| Value | Meaning |
|-------|---------|
| 0 | unknown |
| 1 | notWear (off finger) |
| 2 | wear (on finger) |

### 6.4 userInfo (subCmd=0x04) ◯

Settable from the FlutterApp source: payload would carry user height /
weight / age. We did not exercise this opcode — modifying user info on
the ring without understanding its full effect is risky. Read at your
own risk; write probably fine but unverified.

### 6.5 systemTime (subCmd=0x05) ✓

See §5 step 2. Payload = 6 bytes:

```
[tz_min_lo, tz_min_hi, epoch0, epoch1, epoch2, epoch3]   (all LE)
```

Note: `tz` here is in **minutes**, not quarter-hours. The G2 glasses use
quarter-hours for their own time-sync command. Easy footgun to mix up.

### 6.6 touchStatus (subCmd=0x06) ✗

Probed with empty payload. Ring did not respond. Either:
- The opcode requires a non-empty payload we haven't found
- Or it's a notify-only push direction (ring → host) we'd see only
  on actual touch events
- Or it's not implemented in this firmware

### 6.7 touchSwitch (subCmd=0x07) ✗

Same as touchStatus — silent on probe. The FlutterApp encoder defines
this with a 1-byte enable/disable payload but never calls it.

### 6.8 pairAuth (subCmd=0x08) ✓

See §5 step 1. Payload = `[0x01]`.

### 6.9 otaStart (subCmd=0x09) ⚠

Don't send unless you actually have OTA firmware to push. Triggers the
ring's bootloader path — irrecoverable to brick if mishandled.

### 6.10 advStart (subCmd=0x0A) ✓

See §5 step 3. Payload = 6 bytes, G2 right-temple MAC in BLE address
order (NOT reversed; see §5.1).

### 6.11 getAlgoKeyStatus (subCmd=0x0B) ✓

Send: empty payload.

Response payload: 1-byte status + ASCII-hex key.

```
Example: [00 63 61 31 31 32 32 33 33 78 78 78 78 78 78 78 78 78 78 78 78 78 78 78 78]
        ^^                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        status=0          ASCII "ca112233xxxxxxxxxxxxxxxx"  (placeholder)
```

Observations on our test ring:
- Status byte `0x00` = ok seen (other values not observed; meaning unknown)
- Key is 24 ASCII hex chars = 96 bits
- The first 8 chars (`ca112233` in the placeholder above) are **the last 4 bytes of the ring's MAC** in display order. For placeholder MAC `f8:29:ca:11:22:33`, the last 4 bytes are `ca:11:22:33` → hex `ca112233`. ✓ Verified on real hardware that this prefix derivation holds; substitute your ring's actual last-4 MAC bytes.
- The remaining 16 chars (shown as `xxxxxxxxxxxxxxxx`) are a 64-bit per-device unique seed. Stable across queries on a given ring, never observed to change. Each physical ring has a different value — treat it as confidential per-device material.

The key is plausibly the secret needed for some authentication step
(possibly the ring↔glasses bridge) — see §13 for why we suspect this.

### 6.12 setAlgoKey (subCmd=0x0C) ✗ (intentionally untested)

The encoder builder exists in the FlutterApp but is never called. We
deliberately did not test sending this opcode — overwriting the ring's
algoKey could brick its pairing relationship with the official Even app
account. Treat as off-limits unless you understand its effect.

### 6.13 healthSettings (subCmd=0x0E) ✓

Send: empty payload.

Response payload: 12 bytes, a feature bitmap.

```
Example: [00 00 00 00 01 00 00 00 00 00 00 00]
                        ^^
                        byte[4] = 0x01 — meaning unknown but stable
```

We have not deciphered which bits represent which sensors. Bit 4 (byte 4
value 0x01) is set on our wear-time test ring; we haven't observed it
clearing. Probing with `setSettings` / `report enable` to flip individual
bits has produced no behavioural change (see §7).

### 6.14 systemSettings (subCmd=0x0F) ✓

Same 12-byte shape as healthSettings, different bitmap. Our ring shows
`byte[5]=0x01`. Meaning unknown.

### 6.15 deviceSn (subCmd=0x10) ✓

Send: empty payload.

Response payload: variable-length ASCII serial number, no length prefix
(read until end of payload).

```
Example: [58 58 58 58 58 58 58 58 58 58 58 58 58 58 58]
        =       "XXXXXXXXXXXXXXX"   (placeholder; real value is 15 ASCII chars)
```

Our ring returned 15 chars. Note this is **different from the serial
embedded in `nvRecover`** (see next). Possibly a manufacturer SN vs an
internal device ID. Format is opaque alphanumeric — typically a mix of
digits and uppercase letters per Even Realities production conventions.

### 6.16 nvRecover (subCmd=0x11) ✓

Send: empty payload.

Response payload: ~44 bytes containing an ASCII serial number embedded
in binary metadata.

```
Example payload (44 B):
  [02 74 00 5D 59 59 59 59 59 59 59 59 59 59 00 00 00 00 00 00
   00 00 00 00 00 00 FF FF FF FF FF FF FF FF FF 15 58 58 58 58
   58 58 58 58]

Embedded ASCII at offset 5..14: "YYYYYYYYYY"   (placeholder, real value
                                                 is a 10-char alphanumeric
                                                 device-internal ID)
Trailing ASCII at offset 36..43: "XXXXXXXX"    (placeholder, truncated
                                                 prefix of deviceSn)
```

This response is one of the few that has **CRC32 reported as
mismatched** by our decoder (the firmware seems to compute it differently
or the model length is off by a byte). We accept the payload anyway.

### 6.17 powerControl (subCmd=0x12) ✗

Probed silent. Likely needs a non-empty payload (power-down / restart /
factory-reset codes). Don't bash this blindly without isolating which
payloads do what — at least one of these triples will reset the ring.

### 6.18 packetAck (subCmd=0x7E) 📖

Mentioned in the FlutterApp encoder as a generic ack carrier. We haven't
seen it on the wire from either side.

### 6.19 heartbeat (subCmd=0x7F) ◐

The ring **sends** this opcode TO the host periodically (every ~30 s
when connected). Same payload shape as `deviceStatus`. The FlutterApp
also sends `heartbeat` from host TO ring on the same cadence as a
keepalive — we tried that and saw no acknowledgment back. Sending or
not sending it does not appear to affect connection lifetime in our
tests, but the FlutterApp does it so we mirror.

### 6.20 rghHeartbeat / rghShakeHands / removeRingNotify (0x80 / 0x81 / 0x82) ✗

The "RGH" prefix appears to mean "Ring↔Glasses Handoff" based on the
FlutterApp's enum naming. The FlutterApp encoder defines builders for
all three but **never calls any of them** in any code path we found.
We probed each with empty and 1-byte payloads — all silent on this
firmware. Possibly bridge-related.

---

## 7. health module opcodes — detailed

### 7.1 health/{cmd}/point ✓

Send: empty payload, `notify/get/ok`. Returns the most recent cached
sample of the requested metric.

Response payload layout (8-9 bytes):

```
[0..1]  i16 LE — primary value
[2..5]  u32 LE — Unix epoch seconds
[6]     1 byte — state (1 = "have sample", others observed but unknown)
[7]     1 byte — extra value (1 byte if pLen=8)
[7..8]  i16 LE — extra value (if pLen=9)
```

For **heartRate**, the BPM is in `extra` (offset 7-8), not `value`.
For **temperature**, the temp is in `value` (offset 0-1), probably
scaled ×10 for one decimal place.
For **hrv** and **spo2**, both `value` and `extra` are populated; based
on range checks, the canonical reading is in `extra` for hrv (ms RMSSD)
and `extra` for spo2 (percent), but this is inferred from realistic
ranges, not from a definitive source.

```
hrPoint example payload (8 B):
  [00 00 87 35 F6 69 01 5B]
  value = 0
  ts    = 0x69F63587 = 1777243527 (decode as Unix s, UTC)
  state = 1
  extra = 0x5B = 91 BPM
```

### 7.2 health/heartRate/daily ✓

Send: empty payload.

Response payload: 11-byte header + N records of 4 bytes each.

```
Header (11 B):
  [0]    record count
  [1..2] reserved (00 00)
  [3..6] startTs   u32 LE — earliest sample window (or 0)
  [7..10] endTs    u32 LE — latest sample window

Each record (4 B):
  [0]    HR (BPM) — high confidence; matches `hr point` for record 0
  [1]    UNKNOWN. Initially guessed "hour-of-day UTC" but doesn't fit
         a per-hour-bucket model. Could be slot index, sample sequence,
         or hour with record 0 special-cased.
  [2..3] UNKNOWN. Drift slightly between queries even when byte 0
         (HR) stays constant. Not a simple min/max for the bucket.
```

Record 0 appears to be a "latest sample" overlay rather than a fixed
historical record. We observed record 0's HR byte changing from 85→76
in the same `b1=8` record across queries 30 minutes apart, while the
other records stayed identical.

The FlutterApp's `RingPacketEncoder.parseDaily` interprets these
differently and was developed against firmware v2.1.0_beta_v3 — the
mapping doesn't match ours. The Python codec also flags R1 daily as
"semi broken." So this layout is hand-rolled from our captures only.

### 7.3 health/activity/daily ✓

Send: empty payload.

Response payload: 7-byte header + N records of 7 bytes each.

```
Header (7 B):
  [0]    page marker (0x10 observed)
  [1..2] reserved
  [3..6] base_ts   u32 LE — typically today's midnight UTC

Each record (7 B):
  [0]    slot index (10-minute bins since base_ts)
  [1..2] steps i16 LE
  [3..4] UNKNOWN
  [5..6] kcal i16 LE
```

Slot 54 = +540 minutes from base = 09:00 from midnight UTC. Verified
against wall time during a walking session.

A response may carry a partial trailing record (1-6 bytes); the ring
continues the data in subsequent notify frames. Our parser flags this
with `+N B partial(continuation?)` but doesn't currently stitch
multi-frame responses.

### 7.4 health/{spo2,temperature,hrv,sleep}/point ✓

Same payload shape as heartRate/point (§7.1). See per-cmd notes there
for which slot carries the canonical reading.

### 7.5 health/{spo2,temperature,hrv,sleep}/daily ✗

Returned `status=ack/refuse` on our firmware for some of these. Either
the ring doesn't aggregate these the same way, or daily is HR-only.
Empirical tests had inconsistent results.

### 7.6 health/{cmd}/measure ◐

Theoretically starts a streaming sample session. We saw a single test
session produce a few notify frames over ~30 seconds, then go silent.
Reproducibility was poor and we didn't characterize the stream layout
fully. Treat as "exists but not fully RE'd."

### 7.7 healthSet — sub-opcodes ✗

`reportEnable` (the suspected "turn continuous-push on/off" opcode) was
**bit-bashed against all 256 single-byte mask values**. Zero responded.
Multi-byte masks (`0x0001`, `0x00FF`, `0xFFFF`, etc.) sent at notify/get/ok
were also silent. Could mean:
- The opcode requires `set` rather than `notify` (we don't send `set` to
  unknown opcodes for safety)
- The opcode requires a structured payload we haven't guessed
- This firmware doesn't implement the opcode

---

## 8. sport module ✗

Module byte `0x03`. The FlutterApp defines a sport_messages.dart enum
mentioning `start`, `stop`, `data`. We probed `cmd=0` and `cmd=1` with
`subCmd=0` and `subCmd=1` — all silent on this firmware. May have been
removed in 2.2.0 or never implemented.

---

## 9. testable module ✗

Module byte `0x7F`. Mentioned in the FlutterApp's `R1_MODULE_*` constants
but no encoder builders. We did not probe.

---

## 10. Caching behaviour ✓

The `point` queries return **cached samples** — the ring auto-records
HR/HRV/SpO2 every few minutes during wear and stores the most recent
in RAM. `point` reads that cache.

This means:
- `point` returns immediately (within ~500 ms) but the data may be stale
  by up to several minutes
- The ring does **not** push live telemetry to the host on its own
  initiative (unlike the official phone app's experience, which gets
  updates via the temple's bridge — see §13)
- If you want fresh values, you have to either poll periodically OR
  successfully drive `measure` (see §7.6)

For polling, ~30-60 second intervals are reasonable. The ring's
internal sampling cadence appears to be 5-10 minutes anyway, so faster
polling just returns the same cached value.

---

## 11. nvRecover and the algoKey ✓ (with implications)

These two opcodes return per-device persistent identifiers:

| Opcode | Returns | Example shape (placeholder values) |
|--------|---------|-------------------------------------|
| `getAlgoKeyStatus` | 24-char ASCII-hex key | `ca112233xxxxxxxxxxxxxxxx` (last-4-MAC + 16 hex per-device) |
| `nvRecover` | embedded ASCII serial | `YYYYYYYYYY` (10-char alphanumeric) |
| `deviceSn` | ASCII serial | `XXXXXXXXXXXXXXX` (15-char alphanumeric) |

The `algoKey`'s structure (last-4-MAC-bytes prefix + 8-byte unique tail)
suggests it's an HMAC seed or a device-binding token. We have **strong
suspicion but no proof** that this key is required for a successful
ring↔glasses bridge bond — see §13.

If you're researching this, the algoKey is one of the most interesting
artifacts to dig into. Possible avenues:
- Capture official-app traffic showing whether the algoKey is
  transmitted to the temple during pairing
- Try `getAlgoKeyStatus` from a freshly-reset ring vs a
  previously-bonded ring — does the value change?
- Look for an opcode that takes the algoKey as payload (we suspect
  one of the 0x80/0x81/0x82 bridge opcodes)

---

## 12. R1 ↔ Host runtime status

A summary of what works for a host that just wants to read sensor data:

| Use case | Status |
|----------|--------|
| Connect, auth, time-sync | ✓ Works reliably |
| Read battery / wear / SN / fw version | ✓ Works |
| Poll most recent HR / SpO2 / HRV | ✓ Works (cached values) |
| Continuous live HR streaming | ◐ `measure` exists; not fully reverse-engineered |
| Daily aggregates (HR / activity) | ✓ Works |
| Daily aggregates (HRV / SpO2 / temp / sleep) | ✗ Inconsistent / refused |
| Bond as the ONLY central | ✓ Works |
| Bond as a SECOND central (multi) | ✗ Ring rejects |
| Trigger official ring↔glasses bridge | ✗ Bond initiates but auth fails (see §13) |

For most fitness-tracking use cases you can build today, the polling-
on-30-second-intervals path covers what you need.

---

## 13. The R1 ↔ G2 bridge attempt — what we learned the hard way

This section documents an extended attempt to drive the official
ring-to-glasses bridge from a third-party host (so the glasses' built-in
health UI displays data). **It does not currently work end-to-end.**
We're documenting in detail because:

- The protocol path is largely understood and might be useful to
  others trying the same
- Several common pitfalls are documented so you can skip them
- The remaining gap is identified, even if not solved

### 13.1 The intended flow (per FlutterApp + community docs) 📖

```
Phase 1 (host → glasses):
  AUTHENTICATION       sid=0x80 cmd=4
  PIPE_ROLE_CHANGE     sid=0x80 cmd=5  (tell right temple it's the cmd-arm)
  TIME_SYNC            sid=0x80 cmd=128

Phase 2 (host → ring):
  pairAuth + systemTime + advStart   (the standard R1 setup, see §5)

Phase 3 (host → glasses):
  RING_CONNECT_INFO    sid=0x80 cmd=6  (with ring's MAC reversed + name)

Phase 4 (glasses initiate on their own):
  Right temple connects to ring directly via BLE
  Right temple forwards ring telemetry to host as sid=0x90 / sid=0x91
  Glasses' built-in health UI populates

Phase 5 (host periodic):
  Right temple BASE_CONNECT_HEARTBEAT every 30s on sid=0x80 cmd=14
```

### 13.2 Status of each phase from our testing

| Phase | Status | Evidence |
|-------|--------|----------|
| Phase 1 AUTH | ✓ Works — triggers real BLE LE-SC LTK bonding | Captured `BT_SMP: FOR LE SC LTK IS USED INSTEAD OF STK` log line + temple ack |
| Phase 1 ROLE | ✓ Works | Temple ack'd `PIPE_ROLE_CHANGE magic=224` |
| Phase 1 TIME | ✓ Works | Temple ack'd `TIME_SYNC magic=222` |
| Phase 2 | ✓ Works | Standard R1 setup as in §5 |
| Phase 3 | ✓ Frame received | Temple echoes `connectRing=1` ack |
| Phase 4 (bond) | ✗ **Fails** | See §13.3 |
| Phase 5 | ✓ Works | Temple ack's `BASE_HEARTBEAT magic=220` every 30s |

### 13.3 The remaining failure mode

After Phase 3, the right temple's bridge firmware enters a scan-and-
connect loop, visible to us via subsequent `sid=0x80 RING_CONNECT_INFO`
notifies carrying a `connRet` field:

| connRet | Inferred meaning | Frequency observed |
|---------|------------------|--------------------|
| 0 | idle / no progress | (default when field omitted) |
| 1 | scanning (variant) | rare |
| 8 | **fail-terminal** | repeating every ~10-15s |
| 19 | scanning (variant) | repeating |
| 62 | unknown error | occasional |

The temple **finds the ring** (general adverts) and **attempts to bond**.
The bond attempt fails. Loop repeats. Hours of testing, never once a
success connRet.

Two attempted approaches, both failed:

**Approach A: disconnect from the ring before triggering**
- Theory: ring is single-central, must release it for the temple
- Result: temple actively cycles `connRet=19 → connRet=8` (scan, fail)
  every 10-15s indefinitely

**Approach B: stay connected to the ring during trigger**
- Theory: per the FlutterApp, the host stays connected (multi-central)
- Result: temple acks the trigger but never even attempts to scan —
  silent on connRet entirely. Ring simultaneously refuses our other
  central attempts. Strong evidence the ring is single-central in this
  firmware.

### 13.4 Pitfalls we hit (so you can skip them)

1. **MAC byte order in `advStart`**: pass the temple's MAC in BLE
   address order (MSB first as it appears in `aa:bb:cc:dd:ee:ff`). Do
   NOT reverse it. The FlutterApp's parameter name `macReversed` is
   misleading. We reversed for several days and it broke everything.

2. **MAC byte order in `RING_CONNECT_INFO`**: the ring's MAC sent in
   THIS frame IS reversed. Yes, this is inconsistent with `advStart`.
   Confirmed in FlutterApp source.

3. **5-tap reset on the right temple**: if the temple's bridge has a
   stale lock on the ring from a previous session, the user must
   physically 5-tap reset the right temple to clear it. Otherwise our
   ring scan finds the ring's adverts but our connect attempts time
   out at 30s because the temple has the ring claimed.

4. **`status=set` is a one-way door**: don't bash unknown opcodes with
   `set` semantics. Our self-imposed rule is "only `notify/get/ok`
   for unknown opcodes." A `set` to the wrong subCmd could brick the
   ring's pairing state with the official Even app.

5. **CRC32 must be correct on outbound; CRC16 will always be wrong on
   inbound**: see §2.3.

### 13.5 Why we believe the bridge can't currently be completed by a third party

The temple's bridge firmware appears to require a **shared secret with
the specific ring** before it'll accept the bond. The most likely
candidate is the `algoKey` (§6.11). Possible mechanisms (any of which
would explain our failure):

1. **`algoKey` provisioning**: the official Even app reads the ring's
   `algoKey` and writes it to the temple via some sid we haven't
   discovered. Without this, the temple has no key to authenticate
   with the ring, so the bond fails at the application layer.

2. **Server-issued binding token**: the official app may obtain a
   per-device token from Even Realities' servers tying the ring's
   serial to the temple's serial, distributed via TLS. The FlutterApp
   doesn't implement this and may have only worked in scenarios where
   the official app had previously done it.

3. **BLE-layer bond persistence**: the official app might cause a
   permanent BLE bond between the ring's secure manager and the
   temple, persisting across power cycles. Our `pairAuth` (which is
   only an application-layer handshake, payload `[0x01]`) doesn't
   touch BLE security manager bonding.

The community FlutterApp implementation **does not bridge the gap**:

- It defines a `setAlgoKey` builder but never calls it
- It defines `removeRing` / `unpair` builders but never calls them
- Its README explicitly says the implementation "uses a best-effort
  envelope that may need adjustment after on-device packet validation"

We have no evidence anyone has ever completed this bridge purely from
a third-party host without prior provisioning by the official Even
Realities app. If you have such evidence, please get in touch.

### 13.6 Workaround: render telemetry yourself

Since the official UI path is gated, the practical alternative is:

1. Connect to the ring directly (works ✓)
2. Poll HR / HRV / SpO2 / battery on a periodic schedule (works ✓)
3. Use whatever notification / lens-text command your G2 firmware
   supports to draw the values to the glasses' display yourself
4. Ignore the temple's built-in health widget entirely

This loses the native widget styling but gains complete control and
works today.

---

## 14. Open questions

For anyone continuing this work, the following are unsolved:

1. **What does the temple need to know about a specific ring before
   it'll bond with it?** Is it the algoKey, a server-issued token,
   pre-existing BLE bond state, or something else?

2. **Does any sid=0x80 cmd we haven't tried carry the ring's
   algoKey?** Possible candidates: cmd=9 UNPAIR_INFO, cmd=10/11/12
   (gaps in the enum), cmd=15+ (untested above the heartbeat).

3. **What do connRet values 1 / 19 / 62 actually mean?** We've inferred
   from context that 8=fail-terminal, but the precise codebook is in
   the temple firmware, not in any community RE.

4. **Why does our firmware version reject sport (0x03) and most
   testable (0x7F) probes when the FlutterApp's enum suggests they
   exist?** Possibly removed in 2.2.0.

5. **What's the correct payload for `health/healthSet/reportEnable`?**
   All 256 single-byte values silent. Multi-byte LE/BE variants silent.
   Possibly takes a structured proto, possibly the opcode itself was
   moved.

6. **What's the layout of `health/heartRate/daily` byte offsets 1-3
   per record?** Our parser reads them as `b1 b2 b3` opaque bytes.
   The hour-of-day hypothesis didn't hold up.

7. **Does sleep / temperature / hrv have a daily-aggregated path on
   2.2.0+?** We saw `ack/refuse` more often than not.

---

## 15. Example: full polling session (annotated)

Below is a transcript of a working session using a third-party host on
firmware 2.2.0.0011, formatted for readability. All bytes are real wire
captures.

### 15.1 Connect + auth

```
TX pairAuth ser=1
   00 A2 5A AC 92 64 01 64 01 00 03 00 08 0D 00 A7 64 01

RX pairAuth ack ser=1 status=ack/set/ok pLen=1
   payload[1]=[00]
   raw=[00 A2 5A AC 92 64 01 64 01 00 03 00 08 0D 00 A7 64 00]
        ^^transferType  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                        ^^CRC32          ^^model bytes (12+1)

TX systemTime ser=2 tz=0min epoch=1777743648
RX systemTime ack ser=2 status=ack/set/ok pLen=0

TX advStart ser=3 (target=C8:8D:65:44:55:66 in BLE order)
RX advStart ack ser=3 status=ack/set/ok pLen=0
```

Now the ring is fully set up. Standard async notifies start arriving.

### 15.2 Battery + wear status

```
TX raw 1 0 1                                  (deviceStatus)
RX system/system/deviceStatus ser=4 status=ack/set/ok pLen=7
   payload[7]=[41 02 01 00 00 00 00]
   parsed: byte0=65(maybe-batt%?) wear=wear flag=1 tail=00000000
```

So this ring is ~65% charged, on finger, "ready" flag set.

### 15.3 Heart rate

```
TX hr point                                   (health 1 2)
RX health/heartRate/point ser=5 status=ack/set/ok pLen=8
   payload[8]=[00 00 87 35 F6 69 01 5B]
   parsed: ts=2026-05-02T17:33:59Z state=1 value=0 hr=91 bpm
```

91 BPM, sample timestamped recently.

### 15.4 SpO2 + HRV

```
TX spo2 point                                 (health 2 2)
RX health/spo2/point ser=6 status=ack/set/ok pLen=8
   payload[8]=[00 00 A6 2D F6 69 01 60]
   parsed: ts=2026-05-02T17:00:22Z state=1 value=0 extra=96  → SpO2 96%

TX hrv point                                  (health 4 2)
RX health/hrv/point ser=7 status=ack/set/ok pLen=9
   payload[9]=[00 00 D4 2D F6 69 01 47 00]
   parsed: ts=2026-05-02T17:01:08Z state=1 value=0 extra=71  → HRV 71 (ms?)
```

### 15.5 Identity / firmware

```
TX raw 1 0 2                                  (deviceInfo)
RX system/system/deviceInfo ser=8 pLen=32
   parsed: fw='2.2.0.0011' hw='603MV1.9.3'

TX raw 1 0 16                                 (deviceSn, subCmd=0x10)
RX system/system/deviceSn ser=9 pLen=15
   parsed: deviceSn='XXXXXXXXXXXXXXX'         (placeholder)

TX raw 1 0 11                                 (getAlgoKeyStatus, subCmd=0x0B)
RX system/system/getAlgoKeyStatus ser=10 pLen=25
   parsed: status=0 key='ca112233xxxxxxxxxxxxxxxx'   (placeholder)
```

### 15.6 Activity and HR daily

```
TX hr daily                                   (health 1 1)
RX health/heartRate/daily ser=11 pLen=23
   parsed: count=3 start=2026-05-02T00:00:00Z end=2026-05-02T14:21:21Z
           recs=[{hr=69 b1=8 b2=4A b3=4E},
                 {hr=72 b1=9 b2=57 b3=67},
                 {hr=69 b1=14 b2=50 b3=64}]

TX activity daily                             (health 5 1)
RX health/activity/daily ser=12 pLen=42
   parsed: page=0x10 base=2026-05-02T00:00:00Z recs(5)=[
     {slot=50(+500 min) steps=0  kcal=2  ?=00,00},
     {slot=51(+510 min) steps=7  kcal=20 ?=07,00},
     {slot=52(+520 min) steps=0  kcal=14 ?=02,00},
     ...
   ]
```

That's a complete day's typical interaction — connect, query, get
data, you're done.

---

## 16. Appendix A: opcode quick-reference

### 16.1 system module (module=0x01, cmd=0x00)

| subCmd | Name | Status | Payload (host→ring) | Response payload shape |
|--------|------|--------|---------------------|------------------------|
| 0x01 | deviceStatus | ✓ | empty | 7 B (battery, wear, flags) |
| 0x02 | deviceInfo | ✓ | empty | 32 B (16-B fw + 16-B hw) |
| 0x03 | wearStatus | ✓ | empty | 1 B enum |
| 0x04 | userInfo | ◯ | unverified | unverified |
| 0x05 | systemTime | ✓ | i16 tz_min + u32 epoch | 0 B ack |
| 0x06 | touchStatus | ✗ | empty | (silent) |
| 0x07 | touchSwitch | ✗ | empty | (silent) |
| 0x08 | pairAuth | ✓ | [0x01] | 1 B (= [0x00] = ok) |
| 0x09 | otaStart | ⚠ | DO NOT PROBE | — |
| 0x0A | advStart | ✓ | 6 B MAC, BLE order | 0 B ack |
| 0x0B | getAlgoKeyStatus | ✓ | empty | 1 B status + ASCII-hex key |
| 0x0C | setAlgoKey | ⚠ | NEVER TESTED | — |
| 0x0E | healthSettings | ✓ | empty | 12 B feature bitmap |
| 0x0F | systemSettings | ✓ | empty | 12 B feature bitmap |
| 0x10 | deviceSn | ✓ | empty | variable ASCII |
| 0x11 | nvRecover | ✓ | empty | ~44 B with embedded ASCII serial |
| 0x12 | powerControl | ✗ | empty | (silent; payload-required?) |
| 0x7E | packetAck | 📖 | unverified | unverified |
| 0x7F | heartbeat | ◐ | empty (host TX) | ring TX shape = deviceStatus |
| 0x80 | rghHeartbeat | ✗ | empty | (silent; bridge-related?) |
| 0x81 | rghShakeHands | ✗ | empty | (silent; bridge-related?) |
| 0x82 | removeRingNotify | ✗ | empty | (silent; bridge-related?) |

### 16.2 health module (module=0x02)

| cmd | subCmd 0x01 daily | subCmd 0x02 point | subCmd 0x03 measure |
|-----|-------------------|-------------------|---------------------|
| 0x01 heartRate | ✓ Works (custom layout) | ✓ Works | ◐ Partial |
| 0x02 spo2 | ✗ refused often | ✓ Works | ◐ Untested |
| 0x03 temperature | ✗ refused often | ✓ Works | ◐ Untested |
| 0x04 hrv | ✗ refused often | ✓ Works | ◐ Untested |
| 0x05 activity | ✓ Works (7-B records) | ✗ refused | n/a |
| 0x06 sleep | ◐ Inconsistent | ◐ Inconsistent | ◐ Untested |
| 0x07 healthSet | ✗ All masks tried silent | n/a | n/a |

---

## 17. Appendix B: status-byte values seen on the wire

| Hex | type | method | ack | Common context |
|-----|------|--------|-----|----------------|
| 0x00 | notify | get | ok | Most query frames host TX |
| 0x01 | ack | get | ok | Some ring responses (deviceInfo, etc.) |
| 0x02 | notify | set | ok | systemTime, advStart in some captures |
| 0x03 | ack | set | ok | Most ring acks of host SET frames |
| 0x05 | ack | get | error | Ring couldn't fulfil a query |
| 0x09 | ack | set | refuse | Ring rejected (e.g. unsupported daily) |
| 0x0D | ack | set | notSupport | Opcode doesn't exist |

---

## 18. Appendix C: bridge connRet codes (sid=0x80 cmd=6 from temple)

For anyone debugging the G2-side bridge attempt, here's what we've seen
in `RingInfo.connRet` field of `RING_CONNECT_INFO` notifies from the
right temple.

| Value | Inferred meaning | Notes |
|-------|------------------|-------|
| 0 | idle / not reported | Default when field is omitted from response |
| 1 | scanning (variant) | Seen briefly after temple bridge resets |
| 8 | **fail-terminal** | Temple gave up the bond attempt |
| 19 | scanning (variant) | Most common scanning state |
| 62 | unknown error | Seen alongside failed bond attempts |

We have **never seen a "success" connRet value** in any session. The
temple never reaches a stable bonded state with the ring under
third-party-host control, so we don't know what value means "bond
established."

---

## 19. References

- **FlutterApp (community Flutter port of the Even app)**:
  [github.com/...](https://github.com/) — particularly:
  - `lib/src/protocol/r1_messages.dart` — encoder builders
  - `lib/src/services/r1_manager.dart` — connection state machine
  - `lib/src/services/ring_bridge_coordinator.dart` — bridge handshake
  - `lib/src/core/mac_utils.dart` — MAC byte ordering helpers
  - **README caveat**: the project README explicitly notes the ring
    envelope is "only partially decoded from disassembly... best-effort
    envelope that may need adjustment after on-device packet validation."

- **evenrealities_rev_share Python codec**:
  - `tools/btsnoop_parser/parser.py` — btsnoop log parser
  - `tools/btsnoop_parser/ring1_packet_codec.py` — R1 envelope codec
  - **README caveat**: "R1 parsing is currently semi broken - it
    seems like opcode/command mapping is incorrect"

- **No btsnoop captures** of a working bridge handshake exist in the
  references we have access to. If you find or generate one, it would
  be enormously useful.

---

## 20. Contributing back

If you verify or refute any claim in this document, please update it.
The most valuable contributions would be:

- Confirming or correcting the meaning of `deviceStatus` byte 0
  (battery hypothesis)
- Identifying the missing piece for the bridge handshake (algoKey
  provisioning, server token, BLE bond state, …)
- Filling in any of the "✗ silent" opcodes with the correct payload
- Confirming behaviour on firmware versions other than 2.2.0.0011
- Adding btsnoop captures of a working official-app bridge for
  byte-by-byte comparison

This document represents many hours of empirical testing, but it's a
work in progress. Every "✓" is a fact someone verified once, on one
ring, on one firmware version. Treat it accordingly.
