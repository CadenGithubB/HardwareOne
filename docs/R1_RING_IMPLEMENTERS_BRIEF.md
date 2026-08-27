# Even Realities R1 Ring — Implementer's Brief

**What this is:** enough to write a working R1 client from scratch. Wire format,
handshake, opcode map, payload layouts, and the traps that cost us weeks.

**Scope:** ring firmware **2.2.0.0011**. This device's behavior demonstrably drifts
between firmware revisions — other reverse-engineering efforts targeting earlier
builds disagree with parts of this document, and at least one of them says so in its
own README. Verify against your unit before trusting anything here.

**Provenance:** every claim below was observed directly on hardware we own, via live
BLE captures against our own independently written client. No vendor specification
exists; nothing here comes from firmware extraction, decompilation, or any material
under NDA. Where our findings coincide with public community projects, the findings
are restated from our captures, not copied from theirs — and where a claim rests
*only* on someone else's work, it is either marked unverified or omitted.

Every device identifier in this document is a **placeholder**. Substitute your own.

### Confidence tags

| Tag | Meaning |
|-----|---------|
| ✓ | Verified on live hardware, reproducible across sessions |
| ◐ | Works with caveats, noted inline |
| ⚠ | Hazardous — read the warning before sending |
| ✗ | Sent it, ring stayed silent |

---

## 1. Link layer

**GATT** ✓ — one custom primary service exposing two characteristics: a
write-without-response characteristic for host→ring, and a notify characteristic
carrying everything ring→host. The notify characteristic advertises `canNotify`
only. Subscribe normally; all responses arrive there.

**Address type** ✓ — the ring uses a **Random Static** address (top two bits of the
MSB are `0b11`, i.e. first byte ≥ `0xC0`). A directed connect by MAC must specify
random addressing. Get this wrong and the controller sits there and times out at
~30 s with no other diagnostic — this is the single most common first-day failure.

**Advertising name** ✓ — `EVEN R1_XXXXXX`, where `XXXXXX` is the last three bytes of
the device address in uppercase hex, no separators.

**MTU** ✓ — negotiates to 64 B if you ask. The largest response we've captured is
~80 B, so at the 23-byte default you *will* fragment and must reassemble.

**Single-central** ◐ — **the ring refuses a second concurrent central connection.**
While you hold the link, everything else — including a paired G2 right temple —
times out trying to connect. This is the constraint that shapes the entire
integration; see §7.

---

## 2. Frame format ✓

Every write and every notify is one logical frame:

```
[0]      transferType    — always 0x00 in all observed traffic
[1..4]   CRC32           — over the model bytes, little-endian
[5..]    model           — 12-byte header + payload
```

### Model header

| Offset | Field | Notes |
|--------|-------|-------|
| 0 | version | always `0x64` |
| 1 | module | §3 |
| 2 | moduleVersion | always `0x64` |
| 3–4 | serial (u16 LE) | per-session counter, starts at 1 |
| 5 | status | §4 |
| 6 | cmd | §3 |
| 7 | subCmd | §3 |
| 8–9 | modelLength (u16 LE) | = 12 + payload length |
| 10–11 | CRC16 (u16 LE) | |
| 12.. | payload | up to ~70 B observed |

### Checksums

**CRC32** — polynomial `0x1EDC6F41` (Castagnoli — *not* zlib's `0x04C11DB7`),
init `0`, no reflection, no final XOR. Computed over the model bytes only.

**CRC16** — CCITT/XMODEM-style, init `0xFFFF`. The input **excludes three bytes**:
model byte 4 (serial high), and bytes 10–11 (the CRC16 field itself). So the input
is `model[0..3] ++ model[5..9] ++ model[12..]`.

### ⚠ The inbound CRC16 is always wrong

**The ring emits an incorrect CRC16 on every frame it sends.** Not occasionally —
every single one. If you validate CRC16 on inbound frames you will reject 100% of
the ring's traffic and conclude your parser is broken. Validate CRC32 on inbound,
ignore CRC16, and surface them as two independent booleans.

Outbound is the opposite: **your CRC32 must be correct** or the ring silently drops
the frame with no error. A silent ring usually means a bad CRC32, not a bad opcode.

### Worked example — pairAuth, serial 1

A frame our client emits and the ring accepts:

```
00 A2 5A AC 92 64 01 64 01 00 03 00 08 0D 00 A7 64 01
```

| Bytes | Field | Value |
|-------|-------|-------|
| `00` | transferType | 0 |
| `A2 5A AC 92` | CRC32 | over the 13 model bytes following |
| `64` | version | |
| `01` | module | system |
| `64` | moduleVersion | |
| `01 00` | serial | 1 |
| `03` | status | ack/set/ok — see the note in §4 |
| `00` | cmd | system |
| `08` | subCmd | pairAuth |
| `0D 00` | modelLength | 13 = 12 header + 1 payload |
| `A7 64` | CRC16 | 0x64A7 |
| `01` | payload | the literal byte 1 |

Use this as a self-test fixture for your encoder: if you can reproduce both
checksums byte-for-byte, your envelope is correct.

### Short error frames ◐

Some malformed-input rejections come back as a **6-byte frame with no model header
at all**: `transferType`, then a CRC32 that **echoes the offending request's CRC32**,
then a single error-code byte. The echo lets you correlate the rejection back to the
specific write that caused it — useful, but you must special-case frames shorter
than the 17-byte minimum before your normal decoder rejects them on length. The only
error code we've confirmed is `0x07`; the codebook is undocumented, so log any new
ones you see.

---

## 3. Opcode space

**Modules** ✓ — `0x01` system, `0x02` health, `0x03` sport (✗ every probe silent —
possibly removed in 2.2.0), `0x7F` testable (untested, appears reserved).

The system module uses `cmd=0x00` throughout and differentiates entirely by subCmd.
The health module uses cmd for the metric and subCmd for the access mode.

### System module (module `0x01`, cmd `0x00`)

| subCmd | Name | | Request | Response |
|--------|------|--|---------|----------|
| `0x01` | deviceStatus | ✓ | empty | 7 B — §5.1 |
| `0x02` | deviceInfo | ✓ | empty | 32 B — two 16-B ASCII strings |
| `0x03` | wearStatus | ✓ | empty | 1 B enum: 0 unknown, 1 not-worn, 2 worn |
| `0x04` | userInfo | ◐ | **decode only; do not write** | Captured 12-byte personal profile; field semantics/ranges are not safe enough for production writes. |
| `0x05` | systemTime | ✓ | 6 B — §6 | 0 B ack. **GET is silent** — you cannot read the ring's clock back. |
| `0x06` | touchStatus | ✗ | empty | silent, even while tapping — see §7 |
| `0x07` | touchSwitch | ⚠ | **DO NOT SET** | §8 |
| `0x08` | pairAuth | ✓ | `[0x01]` | 1 B, `[0x00]` = ok |
| `0x09` | otaStart | ⚠ | **DO NOT PROBE** | §8 |
| `0x0A` | advStart | ✓ | 12 B dual-temple identity — §6 | 0 B ack |
| `0x0B` | getAlgoKeyStatus | ✓ | empty | 1 B status + 24-char ASCII-hex key — §9 |
| `0x0C` | setAlgoKey | ⚠ | **NEVER TESTED** | §8 |
| `0x0E` | healthSettings | ◐ | SET epoch + enabled + padding | empty ACK only; no GET/readback proven |
| `0x0F` | systemSettings | ✓ | GET empty; SET epoch + type 0 + enabled + padding | response byte 5 = low power off/on |
| `0x7E` | packetAck | ✓ | 10 B identity + received data serial | host flow-control ACK for valid daily data notifies |
| `0x10` | deviceSn | ✓ | empty | variable-length ASCII, no length prefix — read to end of payload |
| `0x11` | nvRecover | ✓ | — | ~44 B with an embedded ASCII device ID. **Push-only**: emitted unsolicited during setup, GET is silent. Our decoder reports its CRC32 as mismatched; accept it anyway. |
| `0x12` | powerControl | ✗ | empty | silent — almost certainly needs a payload, and at least one of those payloads is a reset. Do not sweep it. |
| `0x7F` | heartbeat | ◐ | — | **Ring→host every ~30 s**, same shape as deviceStatus. Host→ring heartbeats are never acked and had no measurable effect on link lifetime. Cannot be solicited. |
| `0x80`–`0x82` | (bridge-related) | ✗ | empty / 1 B | all silent; naming suggests ring↔glasses handoff |

### Health module (module `0x02`)

cmd = metric: `0x01` heartRate, `0x02` spo2, `0x03` temperature, `0x04` hrv,
`0x05` activity, `0x06` sleep, `0x07` healthSet.

subCmd = access mode: `0x01` daily (aggregated history), `0x02` point (latest cached
sample), `0x03` measure (start a live sampling session).

| cmd | daily | point | measure |
|-----|-------|-------|---------|
| heartRate | ✓ | ✓ | ◐ |
| spo2 | ✓ hourly aggregates | ✓ | untested |
| temperature | ✗ often refused | ✓ | untested |
| hrv | ✓ 16-bit hourly aggregates | ✓ | untested |
| activity | ◐ typed; full-day completeness unproven | ✗ refused | n/a |
| sleep | ◐ empty/no-data semantics observed | ◐ inconsistent | untested |
| healthSet | ✗ | n/a | n/a |

The ring rejects unsupported combinations cleanly with a refuse status rather than
going silent, so **probing GET-shaped requests is safe and cheap.** `measure` needs
several seconds to boot the PPG algorithm before it emits anything.

`healthSet`/reportEnable — the suspected "enable continuous push" knob — was
bit-bashed across all 256 single-byte values plus multi-byte variants at GET
semantics. Every one silent. Either it needs SET semantics (which we won't send
blind), a structured payload we haven't guessed, or it doesn't exist in this build.

---

## 4. Status byte ✓

One byte packing three fields:

| Bits | Field | Values |
|------|-------|--------|
| 0 | type | 0 = notify, 1 = ack |
| 1 | method | 0 = get, 1 = set |
| 2–3 | result | 0 ok, 1 error, 2 refuse, 3 notSupport |

| Value | Decoded | Context |
|-------|---------|---------|
| `0x00` | notify/get/ok | most host queries |
| `0x01` | ack/get/ok | some ring responses |
| `0x02` | notify/set/ok | systemTime, advStart |
| `0x03` | ack/set/ok | most ring acks — **and what the host sends on pairAuth** |
| `0x05` | ack/get/error | ring couldn't fulfil the query |
| `0x09` | ack/set/refuse | valid request, not allowed in current state |
| `0x0D` | ack/set/notSupport | opcode doesn't exist in this firmware |

**Note the oddity:** by the bit layout, the `0x03` the host sends on pairAuth decodes
as *ack*/set/ok — which is a strange thing for a host-originated frame to claim.
It's nonetheless what the ring accepts on this firmware. Don't try to "fix" it to a
notify value; send `0x03` and move on. (Public community documentation labels this
value inconsistently — trust the wire.)

`refuse` means "valid but not allowed right now." `notSupport` means "no such
opcode." The distinction is genuinely useful when probing.

---

## 5. Payload layouts

### 5.1 deviceStatus — 7 B

```
[0]     ⚠ battery percent (inferred, high confidence)
[1]     wear status: 0 unknown / 1 not-worn / 2 worn
[2]     always 0x01 observed — possibly a ready/valid flag
[3..6]  always zero
```

Byte 0 is the strongest inference in this document rather than a confirmed field:
it held constant across nine queries spanning four minutes, and falls in single-digit
steps across days. It behaves exactly like a charge gauge. Byte 1 matches the
dedicated wearStatus opcode exactly.

### 5.2 deviceInfo — 32 B

Two 16-byte null-padded ASCII strings: firmware version, then hardware version.
Trim each half at the first non-printable byte. Ours reports `2.2.0.0011` and
`603MV1.9.3`.

### 5.3 health point — 8 or 9 B

```
[0..1]  i16 LE — "value"
[2..5]  u32 LE — Unix epoch seconds (UTC)
[6]     state  — 1 = sample present; other values seen, unexplained
[7]     u8     — "extra", when payload length is 8
[7..8]  i16 LE — "extra", when payload length is 9
```

**Which slot holds the reading depends on the metric, and this is a real trap.**
For heart rate the BPM is in **extra**, not value. For temperature the reading is in
**value**, apparently scaled ×10. For HRV and SpO2 both slots are populated, and our
reading of extra as canonical (RMSSD in ms; percent) is inferred from plausible
physiological ranges rather than confirmed. Sanity-check against a reference device
before trusting HRV or SpO2 absolutely.

Example — heart rate point, 8 B: `00 00 87 35 F6 69 01 5B` → value 0, timestamp
`0x69F63587`, state 1, extra `0x5B` = **91 BPM**.

### 5.4 heartRate / SpO2 daily (`2.2.7.0005`)

```
Header: [count][timezone i16 LE][dayStart u32 LE][latestTs u32 LE][latest u8]
Record: [hour][average][maximum][minimum]
Trailer: [opaque u32 LE; preserve, do not compare to a fixed sentinel]
```

The count-derived length is part of the integrity check. The ring-owned trailer
varies between sessions (observed examples include `0x0000281F` and
`0x00009AD4`); preserve it as metadata, but do not reject a valid frame based
on a guessed constant.

### 5.5 HRV daily (`2.2.7.0005`)

Same header semantics, but latest/average/maximum/minimum are `u16 LE`; each
record is seven bytes (`hour` plus three 16-bit values). Never decode HRV through
the one-byte common-record path.

### 5.6 activity daily (`2.2.7.0005`)

```
Header: [count][timezone i16 LE][dayStart u32 LE]
Record: [0] slot index — 10-minute bins since base
        [1..2] u16 LE steps
        [3..4] u16 LE active kcal
        [5..6] u16 LE total kcal
Trailer: [opaque u32 LE; preserve, do not compare to a fixed sentinel]
```

Validate slot `0..143` and `total >= active`. The current captures contain only
one/two morning records; mark results partial until a high-cardinality late-day
capture resolves sparse records versus paging/fragmentation.

---

## 6. Session setup ✓

Four protocol stages, each awaiting its completion. **No key exchange, no server-issued token** —
earlier community speculation about a remote pairing key is empirically wrong.

```
1. pairAuth     module 0x01, cmd 0x00, subCmd 0x08, status 0x03
                payload [0x01]                              → ack, then wait ~1 s

2. deviceInfo   module 0x01, cmd 0x00, subCmd 0x02, status 0x00
                empty; require data response firmware 2.2.7.0005

3. systemTime   module 0x01, cmd 0x00, subCmd 0x05, status 0x02
                payload i16 LE tz_minutes ++ u32 LE epoch_seconds
                                                            → ack, then wait ~200 ms

4. advStart     module 0x01, cmd 0x00, subCmd 0x0A, status 0x00
                reverse(right MAC) ++ reverse(left MAC)      → ack, then wait ~200 ms
```

After step 1 the ring acks and begins emitting telemetry if it's being worn.

### ⚠ Two traps in this sequence

**Timezone units.** systemTime takes tz in **minutes**. The G2 glasses' own
time-sync command takes quarter-hours. If you're driving both, you will mix these up
at least once.

**Address shape is profile-specific.** The `2.2.7.0005` official app reverses
each temple address and concatenates right then left. Never substitute a captured
constant, zero address, single-address legacy payload, or phone address.

---

## 7. Behavioral model — read this before designing

**The ring is a pull device.** It does not stream telemetry to a host on its own
initiative. The experience the vendor's phone app gives you comes via the glasses'
bridge, not via anything the ring pushes to a general client.

**`point` reads a cache, not a sensor.** The ring samples autonomously while worn —
roughly every 5–10 minutes — and stores the latest value per metric. Your query
returns in under 500 ms with a value that may be minutes old. **Polling faster than
~30 s returns you the identical sample.** 30–60 s is the sensible range; anything
tighter is wasted radio time.

**You cannot tell a fresh sample from a stale re-read** without doing the work
yourself. Combined with the write-only clock (§3, systemTime), this is the subtlest
problem in the whole integration: nothing in a response distinguishes "the ring just
measured this" from "you're reading the same cached value for the fortieth time," and
naive loggers produce graphs with forty identical points. Record a per-metric
timestamp *and* a provenance flag saying whether that timestamp came from the ring's
clock or from your own receipt clock — and freeze both at receive time, not at write
time, or an NTP step will retroactively rewrite the history of samples that never
changed. Design this in from the start; retrofitting it is genuinely painful.

**Touch events don't come over this link.** Physical taps produce nothing on any
system-module opcode, including while touchStatus is being actively polled. They
appear to route to the glasses over the ring↔glasses link instead. Don't burn time
looking for them here.

### The ring↔glasses bridge is not reachable third-party ✗

If your goal is to make the glasses' *built-in* health widget display ring data:
we could not get there, and we found no evidence anyone has.

Every stage up to the final bond succeeds — the glasses authenticate, accept the
role change, take the time sync, acknowledge the connect-info frame, and the temple
then enters a scan-and-connect loop and **finds the ring**. Then the bond fails. The
temple reports a terminal-failure code roughly every 10–15 s, forever. We never
observed a success code in any session, across two opposite strategies (releasing the
ring first, and holding it during the trigger) and many hours.

The most likely explanation is that the temple requires a **per-ring shared secret**
provisioned by the vendor's own app — the per-device key of §9 is the obvious
candidate — or a persistent BLE-layer bond that an application-layer handshake never
establishes. The single-central property makes the "hold the link" strategy
structurally impossible regardless.

**Practical answer: read the ring yourself and render the values to the lens with
your own display commands.** You lose the native widget styling and gain complete
control. Plan for this from day one rather than treating it as a fallback.

---

## 8. ⚠ Hazards

These are the ones that cost real hardware damage or came close.

**`touchSwitch` (system `0x07`) disables the ring's touch sensor.** We swept payload
values `00`/`01`/`02`/`FF` at set semantics and the ring's touchpad stopped
responding to physical taps entirely. It is also a liar: the response is
byte-identical across every value sent — same CRC32, same CRC16, same serial 0 — so
it's a hard-coded canned ack, **not** a value echo. You cannot read the current state
back. **Recovery is physical** (§10), not a software undo.

**`otaStart` (system `0x09`)** is the Even-app prelude to a Nordic Secure DFU
session (HCI-proven: `otaStart` → disconnect → DFU control/packet upload). Without
a valid **signed** image this is a brick. Don't probe it. Passive capture/RE only —
see [`OTA_PASSIVE_CAPTURE.md`](OTA_PASSIVE_CAPTURE.md) / `tools/btsnoop/r1_dfu_extract.py`.

**`setAlgoKey` (system `0x0C`)** overwrites per-device binding material and may
permanently destroy the ring's pairing relationship with the vendor's app. We
deliberately never sent it, and neither should you.

**`powerControl` (system `0x12`)** is silent on an empty payload, which means it
wants one — and at least one of those payloads is almost certainly a reset or
factory-wipe. Do not sweep it blindly.

**The operating rule that kept us out of trouble: never send set semantics to an
opcode you don't understand.** GET-shaped probes are safe — the ring refuses cleanly.
SET is a one-way door, sometimes into hardware you can't recover in software.

---

## 9. Per-device secrets

Three per-device identifiers are readable over the air with no authentication beyond
the trivial pairAuth:

- **The algo key** — 24 ASCII hex chars (96 bits). The first 8 chars are the last
  four bytes of the ring's own address; the remaining 16 are a per-device 64-bit
  value, stable across queries and unique per ring. Structurally this looks like an
  HMAC seed or device-binding token, and it is our leading suspect for the missing
  piece of the bridge handshake.
- **A device serial** — 15 ASCII chars.
- **A second, different internal ID** — 10 ASCII chars, embedded in the nvRecover
  push.

**Treat all three as confidential.** They are stable device fingerprints. Keep them
out of logs, screenshots, bug reports, and documents like this one — every identifier
in this brief is a placeholder for exactly that reason.

---

## 10. Physical recovery

Not protocol, but you will need it: some SETs leave the ring in a state the wire
protocol cannot recover.

**Five rapid taps on the charging puck** (ring seated, ~1 tap/sec, within 2–3 s)
reinitializes the touch sensor without a disconnect or power cycle. This restored a
touchpad disabled by a `touchSwitch` SET, immediately and completely. Whether the
count is exactly five or just "several rapid taps in a window" is uncharacterized —
we needed it once and it worked.

If a user-facing ring feature stops working after you send a system-module SET, try
this before concluding the ring is bricked.

---

## 11. Known-unknown list

Genuinely open, in rough order of value:

1. What does the temple need to know about a specific ring before it will bond?
   Key provisioning, server-issued token, pre-existing BLE bond state, something else?
2. Does any glasses-side command carry the ring's algo key to the temple?
3. What is the typed daily layout, if any, for skin temperature and sleep?
4. Does a high-cardinality activity day use sparse records, multiple protocol
   pages, or BLE fragmentation?
5. What does deviceStatus byte 0 actually mean? (Battery is an inference, not a
   confirmation.)
6. Full semantics of the `measure` streaming mode — it emits, but we never
   characterized the stream reliably.

---

*Every ✓ in this document is a fact verified once, on one ring, on one firmware
version. Treat it accordingly, and correct it when your hardware disagrees.*

*Independent reverse-engineering of a purchased device for interoperability
purposes. Not affiliated with, endorsed by, or sourced from Even Realities. No
warranty — some opcodes described here can damage hardware.*

*Document revision: 2026-08-03; production profile limited to R1 firmware
`2.2.7.0005` unless a new capture-backed profile is added.*
