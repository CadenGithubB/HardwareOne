# Even Realities G2 Glasses — Protocol Reference

Notes accumulated while bringing up the G2 integration in this repo. Facts
here were verified either against a real pair of G2 glasses over BLE or by
reading the source of https://github.com/Commute773/g2-kit-unofficial (MIT,
TypeScript). Things flagged "reference" come from that repo; things flagged
"observed" came from live traffic against pair `Even G2_32_L_4F5AA0` /
`Even G2_32_R_009769`.

## Naming note

The firmware's internal name for the sid=0xE0 rendering subsystem is
**"EvenHub"** — pb message type `evenhub_main_msg_ctx`, command enum
`EvenHub_Cmd_List`, etc. "EvenHub" is **also** the marketing name of Even
Realities' plugin/SDK platform for third-party mini-apps on the glasses. To
avoid implying a plugin integration (this codebase doesn't use the SDK, it
just speaks the underlying rendering protocol directly), we call the
subsystem **"EvenCore"** throughout our code and this document. Where we
reference the upstream pb schema or the reference implementation, we keep
the firmware identifiers verbatim (`EvenHub_Cmd_List`, `evenhub_main_msg_ctx`,
`EvenHub_pb.ts`) so they grep cleanly against the source.

## Hardware / topology

- Each pair exposes **two independent BLE peripherals** — the left temple and
  the right temple. They do not synchronise internally; a phone/device
  connects to both separately.
- The **right temple is primary**: display commands go there and async
  events (touch, wear, state) come only from there.
- The **left temple** carries the microphone audio stream and emits no
  spontaneous traffic. Heartbeats still need to be sent to it (see below).
- The firmware mirrors rendering to both lenses internally when a display
  command is sent to the right temple. You do **not** send display
  commands to the left.

## BLE advert / name

- Name regex: `/(Even )?G\d+_\d+_[LR]_/`
  - e.g. `Even G2_32_L_4F5AA0`, `Even G2_32_R_009769`
- The `Even ` prefix may or may not be present — match both.
- The pair ID (between the version and `L`/`R`) can be short (observed "32").
- Use **active scan with duplicates enabled** — the name often arrives in the
  scan response, not the primary advert.
- Arduino BLE library emits `parseAdvertisement: Failed to allocate 0 bytes
  for payload` for some unrelated empty-payload adverts during scan. Non-fatal,
  ignore.

## GATT services

Four services live under the Even Realities base UUID
`00002760-08c2-11e1-9073-0e8ac72e____`:

| UUID suffix | Purpose |
|---|---|
| `5450`      | **Command** service (write + notify chars below). This is the one we use. |
| `6450`      | Render / audio. LC3 mic packets notify on `6402` here. |
| `7450`      | Unknown — observed, not yet used. |
| `1001`      | Unknown — observed, not yet used. |

Plus standard Nordic UART (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`) — not used
by the official app protocol.

### Characteristics on service `5450`

| Role | UUID suffix | Properties |
|---|---|---|
| Write (host → glasses) | `5401` | write-without-response |
| Notify (glasses → host) | `5402` | notify |

### Arduino BLE gotcha

When looking up the notify characteristic, Arduino's BLE library walks
descriptors eagerly and logs `retrieveDescriptors: esp_ble_gattc_get_all_descr:
Unknown`. That error is non-fatal — `registerForNotify()` still succeeds and
notifications arrive.

## Transport envelope

**Every** BLE write on the command characteristic uses this envelope. Same
shape for received notifications (with a different preamble byte).

```
TX:  AA 21  seq  len  totFrags  fragIdx  sid  flag  <pb payload>  <crc LE>
RX:  AA 12  seq  len  totFrags  fragIdx  sid  flag  <pb payload>  <crc LE>
     └─┬─┘  └┬┘  └┬┘  └──┬───┘  └──┬──┘  └┬┘  └┬─┘
       │    │    │      │         │      │    └── flag byte (see table)
       │    │    │      │         │      └── subsystem ID (see table)
       │    │    │      │         └── 1-based fragment index
       │    │    │      └── total fragments for this message
       │    │    └── fragment data length — pb bytes + 2 (CRC) on last frag,
       │    │        pb bytes alone on mid-frags. u8; max value 255.
       │    └── transport seq byte. GROUP KEY — every fragment of one logical
       │        message shares the same seq. NOT an incrementing counter.
       └── preamble. 0xAA 0x21 for TX, 0xAA 0x12 for RX.
```

8-byte header. **Not** 11 — earlier docs claiming a u32 magic field in the
header are wrong. The "magic" (`MagicRandom`) lives inside the EvenCore
protobuf payload for ack-correlation, not in the transport header.

### CRC

CRC-16/CCITT-FALSE: `poly=0x1021`, `init=0xFFFF`, no input/output reflect, no
final XOR.

- Computed over **just the pb payload bytes** (not the header).
- Appended **little-endian** (low byte first) to the **last** fragment.
- Mid-fragments have no CRC.

### Fragmentation

- Default chunk size in the reference is 232 bytes of fragment data; MTU-3
  is the practical ceiling.
- The firmware buffers fragments by `seq` and dispatches once it has `len`
  bytes across the whole message.
- Incrementing `seq` per fragment makes the firmware see orphaned tail
  fragments and silently drop the whole message. **Keep seq constant across
  fragments of one message**; increment only between messages.
- In our current implementation every message fits in one fragment, so this
  is moot for now, but mandatory once we add image streaming.

### Flag byte

| Value | Meaning |
|-------|---------|
| 0x20  | REQUEST (host→glasses command). **Not 0x00.** |
| 0x00  | RESPONSE (glasses→host ack) |
| 0x01  | NOTIFY (async event from glasses) |
| 0x06  | NOTIFY alternate — observed, same semantics as 0x01 in practice |

### Subsystem IDs (sid)

| SID  | Channel | Notes |
|------|---------|-------|
| 0x01 | App-launch handshake + **built-in dashboard events** | Prelude lands here; heartbeat acks echo here too; `cmd=3` carries `List_ItemEvent` for the firmware's own dashboard widget — see "Built-in dashboard events" below |
| 0x09 | Settings / telemetry | Battery %, brightness, software version, wear state |
| 0x0D | State change events | Display-state-machine signals (display on/off, user-activity beacon). Coarser than sid=0x01 cmd=3; see "What this channel reports" below |
| 0x0E | Widget transform | Not explored |
| 0x80 | Firmware keepalive | Periodic ~10s ping on the right temple in firmware 2.2.0.242+. Echoes seq byte. New on the OTA, possibly ring-related (correlates with ring-relay channel). Harmless to ignore |
| 0xC5 | Android notification payload | Not explored |
| 0xE0 | **EvenCore** — all display/image/audio rendering (firmware name: EvenHub) | Our main channel |

## App-launch prelude

**Required once per fresh BLE connection.** Send to each arm right after
service/characteristic discovery and before any EvenCore traffic.

Verbatim 27 bytes, copied from the reference's `PRELUDE_F5872`:

```
aa 21 92 13 01 01 01 20  08 02 10 9c 01 22 0a 1a
08 12 06 12 04 08 00 10  00 a1 42
```

- `seq=0x92`, `len=0x13`, `totFrags=1`, `fragIdx=1`, `sid=0x01`, `flag=0x20`
- Inner pb: `type=2`, `magic=156 (0x9c)`, plus deeply-nested zero-varints
  whose semantics are not recovered.
- Ack arrives as `sid=0x01 flag=0x00 pbLen=7` within a few hundred ms.
- Reference settles **800 ms** after send before doing anything else. We do
  the same. Skipping this settle causes subsequent EvenCore commands to fail
  silently.

### Do NOT do

Do **not** send a `CreateStartUpPage` (EvenCore Cmd 0) as part of the prelude.
Doing so reboots the glasses. The reference only sends the byte-literal
above.

## EvenCore (sid=0xE0)

Top-level wrapper is `evenhub_main_msg_ctx` (firmware name — we don't rename
wire-level identifiers). Every EvenCore message is a protobuf encoding of
this type, placed in the envelope's pb payload.

### Wrapper field numbers

| Field # | Name | Type | Purpose |
|---------|------|------|---------|
| 1 | Cmd | varint (enum) | Command kind — see Cmd enum below |
| 2 | MagicRandom | varint | Host-chosen ack-correlation ID. Response echoes it in its own Cmd-response wrapper. |
| 3 | CreateMessage | CreateStartUpPageContainer | (body for Cmd=0) |
| 5 | ImgRawMsg | ImageRawDataUpdate | (body for Cmd=3) |
| 7 | RebuildContainer | RebuildPageContainer | (body for Cmd=7) |
| 8 | RebuildResCmd | ResponseRebuildCmd | (body for Cmd=8, OS response) |
| 9 | TextUpgrade | TextContainerUpgrade | (body for Cmd=5) |
| 11 | ShutDownCmd | ShutDownContaniner | (body for Cmd=9) |
| 14 | HeartPacketCmd | HeartBeatPacket | (body for Cmd=12) |
| 18 | AudioCtrCommand | AudioCtrCmd | (body for Cmd=15) |

Response messages reuse the wrapper with their own Cmd value and their
payload in the matching response field (e.g. Cmd=8 → field 8).

### Decoding response codes

`ResCmdMsg` is **nested**, not a direct varint in the wrapper. Example of a
REBUILD failure response on the wire:

```
08 08 10 CA 01 42 02 08 07
│  │  │  │       │  │  │  │
│  │  │  │       │  │  │  └── varint 7 = RebuildFailed
│  │  │  │       │  │  └── field 1 (ResCmdMsg), varint wire
│  │  │  │       │  └── nested len = 2
│  │  │  │       └── field 8 (RebuildResCmd), len-delim wire
│  │  │  └── MagicRandom varint = 0xCA (202, matches our REBUILD magic)
│  │  └── field 2 (MagicRandom)
│  └── varint 8 = OS_RESPONSE_REBUILD_PAGE_PACKET
└── field 1 (Cmd)
```

`ResCmdMsg` lives at **inner field 1** of every rendering-path response
(`ResponseCreateStartupCmd`, `ResponseRebuildCmd`, `ResponseTextUpgradeCmd`,
`ResponseShutDownCmd`). `ResponseHeartBeatCmd` puts it at inner field 2
(field 1 is `Cnt`), and `ResponseImageRawDataCmd` uses field 8 — heartbeat
and image acks are on separate paths, so the simple "descend once, read
first varint" parser works for everything else.

### Cmd enum (EvenHub_Cmd_List)

| Value | Name | Notes |
|-------|------|-------|
| 0  | APP_REQUEST_CREATE_STARTUP_PAGE_PACKET | Launcher/startup page |
| 1  | OS_RESPONSE_CREATE_STARTUP_PAGE_PACKET | |
| 3  | APP_UPDATE_IMAGE_RAW_DATA_PACKET | Image pixel fragment |
| 4  | OS_RESPONSE_IMAGE_RAW_DATA_PACKET | |
| 5  | APP_UPDATE_TEXT_DATA_PACKET | In-place text update (no geometry change) |
| 6  | OS_RESPONSE_TEXT_DATA_PACKET | |
| 7  | APP_REQUEST_REBUILD_PAGE_PACKET | Primary "show text / list / image" command |
| 8  | OS_RESPONSE_REBUILD_PAGE_PACKET | |
| 9  | APP_REQUEST_SHUTDOWN_PAGE_PACKET | Tear down current container |
| 10 | OS_RESPONSE_SHUTDOWN_PAGE_PACKET | |
| 12 | APP_REQUEST_HEARTBEAT_PACKET | Every 5 s — see heartbeat section |
| 13 | OS_RESPONSE_HEARTBEAT_PACKET | |
| 15 | APP_REQUEST_AUDIO_CTR_PACKET | Mic start/stop |
| 16 | OS_RESPONSE_AUDIO_CTR_PACKET | |
| 19 | APP_REQUEST_OPEN_IMU_PACKET | IMU data reporting |
| 20 | OS_RESPONSE_IMU_PACKET | |

### Response error codes (EvenHub_ErrorCode_List)

Inside response bodies (e.g. `RebuildResCmd.ResCmdMsg`).

| Value | Name |
|-------|------|
| 0  | CreatePageSuccess |
| 1  | InvalidContainer |
| 2  | OversizeResponse |
| 3  | OutOfMemory |
| 4  | ImgRawSuccess |
| 5  | ImgRawFailed |
| 6  | RebuildSuccess |
| 7  | RebuildFailed |
| 8  | TextSuccess |
| 9  | TextFailed |
| 10 | ShutdownSuccess |
| 11 | ShutdownFailed |
| 12 | HeartbeatSuccess |
| 13 | AudioCtrSuccess |
| 14 | AudioCtrFailed |

### TextContainerProperty fields (flat — no Rect/TextStyle submessages)

| # | Field | Notes |
|---|-------|-------|
| 1 | XPosition | Default 0 |
| 2 | YPosition | Default 0 |
| 3 | Width | Full lens = 576 |
| 4 | Height | Full lens = 288 |
| 5–8 | BorderWidth, BorderColor, BorderRadius, PaddingLength | |
| 9 | ContainerID | Pick any; 1 works |
| 10 | ContainerName | **≤14 chars, case-sensitive**. Reused name → in-place update |
| 11 | IsEventCapture | 0 for text (reference comment: "we don't tap text areas") |
| 12 | Content | UTF-8, cap ~1000 bytes; `\n` is a line break |

Fonts are fixed LVGL; no font-size API. Plan on an empirical **50×10
character grid** per lens.

### Image geometry (for future reference)

- Full lens: **576×288 pixels**, rendered as a **2×2 grid of 288×144 tiles**.
  Larger tiles choke the firmware.
- Pixel format: **4-bpp indexed**, 16-entry palette. Treat as monochrome
  (0 off, 15 on) unless dithering.
- Row-major, top-down, 2 px per byte (high nibble first).
- Stream each tile in ≤4 KB fragments via Cmd=3 `UpdateImageRawData`.
- First image after CREATE is silently dropped — push a sacrificial warmup
  frame.
- Maintain a 4-write sliding window and tolerate ≤3 consecutive missing acks.
- Observed ceiling ~8.8 KB/s (≈5 FPS full-lens). Ack latency dominates, not
  BLE wire time.

### Image-streaming empirical constraints (firmware 2.2.0.24)

These were learned the hard way running the `g2ProbeImageQ*` series in
2026-04-26. Independent of (and additional to) the schema documented
above:

#### BMP format is mandatory for `MapRawData`

The firmware does **format detection** on the pixel-data buffer. Only
input starting with the `BM` (`0x42 0x4D`) magic — i.e. a real BMP file
— is accepted; raw 4-bpp nibble streams get silently rejected at the
render layer. Source: jimrandomh on the G2/R1 dev Discord (2026-04-25),
corroborated by our `g2ProbeImageQ5BmpRender` returning `cmd=4
ImgRawSuccess` only after we wrapped pixel data in a 4-bpp BMP (file
header + DIB header + 16-entry BGRA palette + pixel rows).

The doc's earlier "4-bpp indexed, 2 px/byte high nibble first, row-major
top-down" description was about the *encoding inside a BMP*, not a raw
wire format. To produce a renderable image:

1. Build a 4-bpp BMP in memory with `biBitCount=4`, `biCompression=0`,
   16-entry palette, row stride padded to 4 bytes. `biHeight` may be
   negative (top-down) or positive (bottom-up — orientation behaviour
   on lens unverified).
2. Send the BMP bytes as `MapRawData` of `Cmd=3 ImgRawMsg`.

`g2-kit-unofficial` source confirms the firmware also has a
`CompressMode` field that does nothing: ~100 values were probed, all
were no-ops. Hardcode `0`.

#### `MagicRandom` for image CREATE must fit in `uint8` (≤ 255)

Empirically observed across multiple sessions: `Cmd=0 CREATE_STARTUP`
with an `ImageObject` body silently drops if `MagicRandom > 255`.
Byte-identical bodies that differ ONLY in the magic value get
opposite outcomes:

| MagicRandom | Varint encoding | CreateResp |
|---|---|---|
| 242 | `F2 01` (fits in uint8) | `res=0 CreateSuccess` |
| 258 | `82 02` (overflows uint8) | silent drop |
| 274 | `92 02` | silent drop |
| 290 | `A2 02` | silent drop |
| 306 | `B2 02` | silent drop |
| 322 | `C2 02` | silent drop |

Same `widgetId=10509`, same `CID=2`, same `name="imgQ4"`, same
`288×144` dimensions, same Q4-shaped pb body — only magic differs and
only the ≤255 case acks. Verified after a fresh ESP32 reboot (so it's
not session-history-dependent on our side). The firmware's image-CREATE
handler appears to truncate magic to a single byte or range-check it
against `uint8` bounds, and the truncation/check causes the entire
request to be discarded silently rather than returning an error.

For CREATE-image, **always pick a magic in the 200–255 range**.
Suggested allocation that stays clear of the existing `G2_MAGIC_*`
constants (201–215) and the heartbeat range (~205): use offsets
between 0x10 and 0x2D from `G2_MAGIC_IMAGE_BASE=210` — i.e. magics
226–255 — for image CREATEs.

**CORRECTION 2026-04-27:** the uint8 constraint applies to push
magics too, not just CREATE. Earlier Q6 testing only reached magic
255 (within uint8) and we incorrectly inferred pushes were
unconstrained. Q10 with 21 push magics crossed the boundary at
magic 256 and got silent-dropped from that point on:

| Magic | Wire (varint) | ImageRawResp ack |
|---|---|---|
| 251–255 | 1 byte (`F2..FF`) | yes |
| 256 | 2 bytes (`80 02`) | **no** |
| 257..271 | 2 bytes | **no** |

So **every magic** in the image-streaming path — CREATE, push, every
fragment — must be ≤ 255. The firmware appears to truncate
(read-as-uint8) and either rejects the body or fails the magic-match
on the response side, in both cases producing a silent drop.

For long streaming sequences (e.g. Q10's 22-slot CREATE+pushes), pack
magics tightly: e.g. CREATE=226, A=227..233, black=234..240,
B=241..247. The full 200..255 range gives 56 slots, plenty for any
realistic per-session sequence.

#### Render only fires when BMP dimensions match container dimensions

Confirmed 2026-04-26 across Q5 / Q6 / Q7:

| Probe | Container | BMP shipped | Cmd=4 ack | Lens render |
|---|---|---|---|---|
| Q5 | 288×144 | 32×32 stripes | yes (magic 247) | **nothing** |
| Q7 | 288×144 | 32×32 all-black | yes (magic 227) | **nothing** |
| Q6 | 288×144 | 288×144 stripes | yes (magics 249..255) | **stripes visible** |

The firmware **acks** smaller-than-container BMPs (so the format is
accepted and parsed), but **does not commit the render** unless the
BMP fully fills the declared container. There's no "draw at offset"
or "partial update" semantics — every frame is full-tile-or-nothing.

Practical consequence: the unit of update is a tile (288×144). To
draw a 32×32 sprite into the corner of an otherwise-black field, you
build a 288×144 BMP with 32×32 of stripes in the top-left and the
rest filled with palette index 0. You cannot push just the 32×32 and
expect the firmware to leave the rest alone.

This also explains the "throughput floor ~2.5 s for a full-lens 4-tile
update" claim — full-tile is the *only* shipping unit. Q6 took ~6.5 s
end-to-end for one tile (CREATE + 7 fragments + render); the discord
2.5 s claim was probably with a tighter pipeline or a different
firmware revision.

#### Multi-fragment Cmd=3 confirmed end-to-end

Q6 (2026-04-26) shipped a 20854 B BMP as 7 chunks of ≤3000 B each,
fragmented at the BLE-MTU layer into 13–14 BLE writes per chunk
(`TX env total=240 ... N/14`). Every chunk got a clean `cmd=4 ack`
with no retransmits. Total wall-clock from CREATE to last ack: ~2.6 s
(155244 ms → 157470 ms in log). Then ~3.0 s of post-render hold +
SHUTDOWN before the picker came back.

The fragment session uses:
- A single `MapSessionId` (Q6 used `1`) shared across all fragments
- Sequential `MapFragmentIndex` 0..N-1
- `MapTotalSize` = total BMP bytes on every fragment
- Each fragment carries a slice of the BMP starting at
  `fragIdx * chunkSize`, sized `chunkSize` except the last one which
  carries the remainder

Each fragment gets its own `MagicRandom` (we used CREATE+1, CREATE+2,
... so push magics for Q6 ran 249..255 — the firmware acks each
individually, no consolidation).

#### Other claims still under test

The following came from the same Discord thread and remain partially
validated. Captured here so future work doesn't have to rediscover.

- **Length-mismatch zero-padding.** Declaring envelope `len` greater
  than the actually-transmitted bytes causes the firmware to zero-pad
  up to the declared length. Useful as a crude RLE for runs of zero
  pixels at fragment boundaries. Untested by us.
- **Two ACK layers, both optional.** BLE-GATT layer (toggleable via
  `WRITE_TYPE_NO_RESPONSE`) and the application/protobuf layer
  (toggleable by setting `MagicRandom = 0`, which makes the firmware
  skip the response packet entirely). Combining magic=0 with rapid
  `Cmd=3` sends overflows a headset buffer and drops the BLE
  connection — hard "do not do" if you're flooding.
- **All-black faster ack.** Weak support 2026-04-26: Q7 (32×32
  all-black) acked at TX→ack ≈138 ms vs Q5 (32×32 stripes) at ≈165 ms
  — a ~16% speedup. Both were sub-container-size pushes that didn't
  render, so the comparison is only of the parse path, not render
  path. Need a re-test with full-tile black vs full-tile patterned
  (Q6-shape) to measure the real render-time delta.
- **4-tile sync trick.** When updating all 4 tiles (288×144 each) of
  a full-lens 576×288 image, send all-but-last fragment of each tile
  first, then send the final fragments. The firmware flips all four
  on the same redraw, avoiding visible per-tile tear. Untested by us
  — only one tile rendered so far (Q6).
- **Throughput floor.** Discord claim was ~2.5 s for a full-lens
  4-tile update. Our Q6 single-tile measurement: ~2.6 s from CREATE
  to last fragment ack, with no overlap. Suggests their figure
  required heavy pipelining (overlap CREATE/fragments across tiles)
  to hit. Worth re-testing once a 4-tile pipeline is in place.

#### Streaming confirmed: multiple Cmd=3 sessions on a single CREATE

Confirmed 2026-04-27 by Q11: after a successful `CREATE-image`, the
firmware accepts an unlimited (tested: 2) sequence of Cmd=3 push
sessions targeting the same container without requiring a fresh
CREATE between frames. Each session uses its own `MagicRandom` and
fragment indices `0..N-1`; the firmware acks every fragment with
`cmd=4 ImageRawResp` and re-renders on the last fragment of each
session, **provided every magic stays ≤ 255** (see uint8 correction
below).

Per-frame cost in our Q11 run (no pipelining, default 100 ms
inter-fragment delay):

| Phase | Wall-clock |
|---|---|
| CREATE-image + ack | ~100 ms |
| Frame A (7 frags + acks) | ~2.64 s |
| Frame B (7 frags + acks, no CREATE) | ~2.65 s |

So the streaming pipeline saves ~100 ms per frame vs the full
SHUTDOWN/CREATE/PUSH cycle, not the seconds we'd hoped for. The
real bottleneck is per-fragment ack pacing — which the discord
"4-tile sync trick" is supposed to compress (untested by us yet).

Implication for feature code: a "live HUD" that pushes a fresh
frame every ~3 s is feasible (~0.4 FPS). Anything snappier needs
the pipelining trick or smaller-than-tile updates (which the
firmware doesn't support — see "Render only fires when BMP
dimensions match container dimensions" above).

#### BLE write transient — `esp_ble_gattc_write_char rc=-1`

Confirmed across Q11 / Q10 / QGlizzy runs (2026-04-27): the
ESP-IDF BLE GATT client occasionally rejects a `writeValue` call
with `rc=-1` ("Unknown ESP_ERR error") mid-burst. Empirical rate:
roughly once per ~100 BLE writes. The error is a **synchronous
queue-refusal** — no bytes hit the radio, so a retry is sending
the failing fragment for the first time, not retransmitting.

If unhandled, the failed write strands the application-level
`t.writeMutex`, which then cascades into:
- subsequent envelope sends timing out on the mutex,
- post-probe SHUTDOWN failing,
- picker-rebuild CREATE-list failing,
- firmware tearing down everything with `SYSTEM_EXIT reason=0`.

**Fix in `sendEnvelopeNoMutex`**: on `rc=-1`, sleep 50 ms and retry
the same write once. The 50 ms gives the BT controller's TX queue
time to drain. Logs surface as:

```
writeValue transient fail at offset N/M — retrying after 50 ms
writeValue retry OK at offset N/M
```

This single retry catches essentially all of the transients we've
observed; sustained failure (link genuinely broken) still falls
through to the existing error path. Average cost: ~50 ms × 1/100
writes ≈ negligible vs the ~2.6 s baseline for a full-tile burst.

#### Path A status (single-tile centred display)

End-to-end stack confirmed working 2026-04-27:

| Layer | Helper |
|---|---|
| BMP file → SD/LittleFS load | `readBmpFromVfs` (BM / 4bpp / 288×144 validation) |
| Palette tuning (brightness/contrast) | `applyBmpPaletteTuningInPlace` |
| CREATE-image protocol | `g2BuildCreateImage` (magic ≤ 255) |
| Multi-fragment Cmd=3 push | `sendImageBmpMultiFragment` (magic ≤ 255 each) |
| Streaming (no re-CREATE between frames) | `sendImageBmpFragmentsNoCreate` |
| Tap-to-dismiss with 60 s safety cap | `probeHoldUntilTapOrTimeout` (DOUBLE_CLICK only) |
| BLE write reliability | retry-on-`rc=-1` in `sendEnvelopeNoMutex` |
| Web/CLI hook | `g2bmp <path> [bright] [contrast] [holdSeconds]` |

Probe coverage: Q4 (CREATE canary), Q6 (full-tile baseline), Q6b
(tap-dismiss variant), Q9 (frame builder), Q10 (3-frame
clear-then-push), Q11 (2-frame swap), QGlizzy (SD-loaded canary).

#### Image-up dismissal: only DOUBLE_CLICK fires SysEvents

Confirmed 2026-04-27 (firmware 2.2.0.24) by Q6b: while a pure image
widget is the only thing on-lens (no list / text widget), the firmware
emits **SysEvent type=DOUBLE_CLICK(3)** on user double-tap (ring or
temple, src=2 for ring, src=0 for temple) but does **not** emit
single CLICK(0) or SCROLL events. So if a probe wants user-initiated
dismissal of an image, the only working gesture is double-tap.

Practical implication for any "show image until user dismisses" flow:
plan on double-tap as the dismiss gesture and add a safety timeout
(we use 60 s in `probeHoldUntilTapOrTimeout`) so a forgotten image
doesn't lock up the picker. Single-tap responsive UIs require either
overlaying a list/text widget on top (which we don't currently do
because it occludes the image) or future firmware changes.

## Heartbeat

- Cmd=12 on sid=0xE0, **every ~5 seconds**.
- `HeartBeatPacket { Cnt = <uint32> }` — firmware ignores the value, the
  packet itself is the heartbeat.
- **Send to both arms.** Each temple runs its own plugin task; without a
  heartbeat the task dies in ~10 s, and subsequent EvenCore commands are
  silently dropped.
- Heartbeat alone does NOT prime the plugin task for the first time — it
  only keeps an already-running task alive. The first REBUILD/CREATE after
  AppLaunch is what primes it.
- Observed ack: each heartbeat echoes back on **sid=0x01 flag=0x01** (not
  0xE0) with a short 6-byte pb carrying `Cmd=12, magic=1, field 14 empty`.
  This is normal.
- **Heartbeat ack tail (sid=0xE0 flag=0x00, this firmware).** On firmware
  2.2.0.24 we additionally observe a longer 11-byte ack on sid=0xE0
  with body `08 0C 10 CD 01 7A 04 08 NN 10 0C` decoding to
  `{cmd=12, magic=205, field 15 = {f1=NN, f2=12}}`. Confirmed
  empirically over hundreds of heartbeat cycles: `f1` (the inner
  sub-field) is a monotonically increasing counter starting at the
  heartbeat number, `f2` is a constant 12 (cmd echo). No hidden
  telemetry. Decoded by `g2DecodeHeartbeatAckTail` in
  `System_G2_Protocol.cpp`; logged behind `DEBUG_G2_DUMP`.

## Settings (sid=0x09)

`G2SettingPackage` wrapper, field numbers:

| # | Field | Purpose |
|---|-------|---------|
| 1 | commandId | enum |
| 2 | magicRandom | |
| 3 | deviceReceiveInfoFromApp | |
| 4 | deviceReceiveRequestFromApp | Used BOTH ways (request and response body) |
| 5 | deviceSendInfoToApp | Async telemetry pushes arrive here |
| 6 | deviceRespondToApp | Sync response body |

`commandId` values: 1 = DeviceReceiveInfo, 2 = DeviceReceiveRequest,
3 = DeviceSendToAPP, 4 = DeviceRespondToAPP, 5 = AppRespondToDevice.

`APPRequestSettingType`: 0 = brightness, 1 = basic (battery etc.).

`DeviceReceiveRequestFromAPP` (used both ways):

| # | Field | Notes |
|---|-------|-------|
| 1 | settingInfoType | enum APPRequestSettingType |
| 2 | autoBrightnessLevel | |
| 3–4 | yCoordinateLevelRestored / xCoordinateLevelRestored | Lens alignment |
| 5–6 | leftSoftwareVersion / rightSoftwareVersion | strings |
| 7–9 | head-up settings | |
| 10 | wearDetectionSwitchRestored | |
| 11 | deviceRunningStatus | |
| **12** | **battery** | 0–100 % |
| 13 | chargingStatus | |
| 14 | silentModeSwitchRestored | |
| 15–16 | left/right calibration | |
| 17 | headUpRecalibrationSuccess | |
| 18 | autoBrightnessSwitchRestored | |
| 19 | unreadMessageCount | |

To query battery: send `G2SettingPackage{commandId=2, magicRandom=N,
deviceReceiveRequestFromApp={settingInfoType=1}}`. Response carries the
`battery` field at the same field number inside `deviceRespondToApp`.

### `deviceSendInfoToApp` (cmd=3 DeviceSendToAPP, async push)

When the user triggers an event the firmware wants to broadcast (rather
than respond to a query), it pushes `G2SettingPackage{commandId=3,
magicRandom=N, deviceSendInfoToApp={…}}` on `sid=0x09 flag=0x01`.

Inner schema (partial — only fields confirmed empirically; numbers
collide with `DeviceReceiveRequestFromAPP` but the meanings differ):

| # | Field | Notes |
|---|-------|-------|
| **2** | **silentMode** | varint 0/1 — pushed every time the user
toggles silent / DND via tap-and-hold-both-temples gesture |

On-wire example (silent ON), captured 2026-04-26:

```
08 03 10 18 2A 02 10 01
│  │  │  │  │  │  │  │
│  │  │  │  │  │  │  └── inner f2 (silentMode) = 1
│  │  │  │  │  │  └── inner f2 wire varint
│  │  │  │  │  └── outer f5 nested len = 2
│  │  │  │  └── outer f5 (deviceSendInfoToApp) wire 2
│  │  └── magicRandom = 0x18
│  └── commandId = 3 (DeviceSendToAPP)
└── outer f1
```

Silent OFF replaces the trailing `01` with `00`.

Parsed by `g2ParseSettingSilentMode` in `System_G2_Protocol.cpp`;
mirrored locally as `gSilentMode` and surfaced in the `g2-status` SSE
as `sm` (-1 unknown, 0 off, 1 on). Other fields of `deviceSendInfoToApp`
are unmapped — future labelled captures (e.g. while charging,
removing/wearing glasses) should reveal which inner fields carry which
state changes.

## State events (sid=0x0D)

Arrive as `flag=0x01` notifications. **Both arms emit the same event
simultaneously** — caller-side dedupe needed. Schema:

```
Outer: { field 1 = varint (always 1)
         field 3 = len-delim nested SysEvent }

SysEvent:
       { field 1 = varint EventType (optional)
         field 2 = varint EventSource (optional)
         ... }
```

### What the channel actually reports

Empirical findings from labelled hardware captures (2026-04-24). The
channel is **coarse-grained** — it reports display-state transitions and
"user input happened," but does NOT distinguish gesture types.

| Payload bytes | Meaning |
|---|---|
| 6 B `08 01 1A 02 08 01` | `SysEvent{EventType=1}` — **user input happened.** Fires for head-up wake, single tap, double tap, swipe up/down — all identical bytes. |
| 4 B `08 01 1A 00` | `SysEvent{}` — **display transitioned on → off.** Fires for BOTH inactivity timeout and user-initiated off (e.g. double-tap-off). |
| 8 B `08 01 1A 04 08 01 10 03` | `SysEvent{EventType=1, EventSource=3}` — user-input event with an explicit source. Per the reference's `EventSourceType` enum: **0=unknown, 1=glasses RIGHT, 2=ring, 3=glasses LEFT**. So `src=3` is **glasses left temple**. **Confirmed 2026-04-25** against firmware 2.2.0.242: ring-only gestures (e.g. ring double-tap on a TEXT widget) fire SysEvent on sid=0xE0 with `src=2`, glasses-temple gestures fire `src=1` or `src=3`. The mapping is correct. |
| 7-9 B with inner `f1 = 4094` | Custom-app events — only fire when a third-party Even mini-app is running. Ignore for built-in gesture handling. |
| 7 B `08 01 1A 03 08 E0 01` | Inner code `224` (`0xE0`). Empirically correlates with **firmware UI transitions over our hijacked widget** — fires around tap-and-hold entering the "Exit?" confirmation dialog, clicking NO/YES, or display-off sequences that target a widget we own. Not a gesture code per se; treat as "firmware sub-UI is active." |
| 7 B `08 01 1A 03 08 8A 02` | Inner code `266` (`0x10A`). **Tap-and-hold-both-temples gesture** — the silent-mode toggle trigger. Fires immediately when the user starts the hold, then ~3 s later the firmware emits a `sid=0x09 cmd=3 deviceSendInfoToApp{f2=0\|1}` push announcing the new silent state. Use this code as the "gesture detected" signal if you want to react before the state push lands. Decoded as `tap-hold-both code=266 (silent-mode toggle)` by the hint formatter. Confirmed 2026-04-26. |
| 9 B `08 01 1A 05 08 E0 01 10 22` | Code `224` with extra `field_2 varint = 34` (`0x22`). Same "firmware UI transition" bucket as the 7B code=224 frame, with additional context byte we haven't decoded. |
| 8 B `08 01 1A 04 08 01 10 04` | `SysEvent{EventType=1, EventSource=4}` — **user-input event from the notification widget.** Observed 2026-04-26 firing while the user clicked through stacked notifications on the dashboard. `src=4` extends the EventSourceType mapping beyond the reference's 0-3 set; treat as "notification panel" until a counter-example surfaces. Pairs with `sid=0x01 GESTURE codes={4,5}` on the same gesture (see "Built-in dashboard events"). |

### Wire shape (decoded from g2-kit-unofficial/ble/events.ts)

The reference treats sid=0x0D events as a hand-parsed pb where:
- field 1 = `1` (constant marker)
- field 3 = nested `SysEvent`, length-delimited:
  - field 1 = subsystem ID (varint, present on most frames)
  - field 2 = optional event code

Our table above is the same pb decoded ad-hoc — both interpretations
agree on the bytes. Use the field-numbered view when stripping new
frames; use the table when looking up well-known patterns.

### What we *cannot* tell from this channel alone

- Which specific gesture the user performed (tap, double-tap, swipe, head-up
  all emit identical `{EventType=1}` bytes).
- Which direction a swipe went.
- Whether the user initiated the display-off or it timed out.

The reference's `OsEventTypeList` enum
(`0=CLICK, 1=SCROLL_TOP, 2=SCROLL_BOTTOM, 3=DOUBLE_CLICK, 4=FG_ENTER,
5=FG_EXIT, 6=ABNORMAL_EXIT, 7=SYSTEM_EXIT, 8=IMU_DATA_REPORT`) is **valid
and matches firmware behaviour** — but it's emitted on the
`sid=0x01 cmd=3` "App" channel for the firmware's own built-in dashboard
widget, NOT on sid=0x0D. The sid=0x0D events documented above are
display-state-machine signals, deliberately coarser. See "Built-in
dashboard events" below for the full schema and decoder.

### What this channel IS good for

- **"Display is currently on"**: you saw a 6-byte event, no 4-byte
  follow-up within a few seconds.
- **"Display just went off"**: 4-byte empty event arrived.
- **"User interacted with the glasses"**: any 6-byte event — treat as
  wake/attention signal regardless of which gesture caused it.

## Built-in dashboard events (sid=0x01 cmd=3)

When the user is interacting with the firmware's **own** built-in
dashboard widget (the menu you scroll through with the touchpad before
tapping a built-in app like Blocks), navigation events flow on
**sid=0x01 flag=0x01 cmd=3** carrying the same `List_ItemEvent`
sub-message we already see for events on **our** hijacked widget on
sid=0xE0 cmd=2. Same widget kind, different "owner" — ours fires on
sid=0xE0 because we own the container; the firmware's dashboard fires
on sid=0x01 because the firmware owns it.

Discovered while testing ring + temple gestures against firmware
2.2.0.242. Not present in older docs; partially decoded in the
g2-kit-unofficial reference's `EvenHub_pb.ts`.

### Wire shape

```
sid=0x01 flag=0x01 cmd=3
  field 1 = 3                     (Cmd)
  field 2 = 0x12345678            (sentinel "no app-correlation"; not a real magic)
  field 6 = body, len-delim:
    field 1 = counter             (varint, monotonically increments per
                                   event — shared across f3/f5 body variants)
    field 5 = List_ItemEvent, len-delim:
      field 1 = 1                 (always — ContainerID? marker?)
      field 2 = nested:           (the actual event detail)
        field 1 = CurrentSelectItemIndex   (varint, only present when meaningful)
        field 2 = EventType                (varint, OsEventTypeList enum)
```

`OsEventTypeList` values — context-dependent per-widget behaviour:

The same enum is used across multiple sub-message types (List_ItemEvent,
Text_ItemEvent, Sys_ItemEvent), and the **firmware emits different
event values for the same physical gesture depending on which widget
is foreground**. This was a confusing finding to nail down (2026-04-25
against 2.2.0.242):

| Value | Name | Where seen |
|---|---|---|
| 0 | `CLICK_EVENT` | Not observed yet — neither dashboard List_ItemEvent nor TEXT-widget SysEvent fires this in 2.2.0.242 |
| 1 | `SCROLL_TOP_EVENT` | **Dashboard widget**: scroll-up at top OR any tap-class gesture (the firmware emits this as the *side-effect* of "list scrolled to top," even when the user actually tapped). **TEXT widget**: not observed |
| 2 | `SCROLL_BOTTOM_EVENT` | **Dashboard widget**: scroll-down advancing into a panel. Carries `CurrentSelectItemIndex` |
| 3 | `DOUBLE_CLICK_EVENT` | **TEXT widget on sid=0xE0 SysEvent**: ring double-tap fires this with `src=2` (verified). **Hijacked List widget on firmware 2.2.0.24**: double-tap ALSO fires `SysEvent DOUBLE_CLICK(3) src=2` on sid=0xE0 — distinct from the single-tap ListEvent CLICK(0) channel (verified 2026-04-27). The earlier claim "Dashboard List_ItemEvent: never observed" applies to the firmware-owned dashboard list on 2.2.0.242 only; on 2.2.0.24 our hijacked List widget surfaces double-tap cleanly. **Practical use:** distinct channels mean we can map single-tap to row selection AND double-tap to a separate gesture (e.g. "manual refresh on a live status page"). |
| 4 | `FOREGROUND_ENTER_EVENT` | sid=0xE0 SysEvent — firmware-owned UI overlay covering our widget (e.g. "Exit?" confirm dialog) |
| 5 | `FOREGROUND_EXIT_EVENT` | sid=0xE0 SysEvent — overlay dismissed |
| 6 | `ABNORMAL_EXIT_EVENT` | Not observed |
| 7 | `SYSTEM_EXIT_EVENT` | sid=0xE0 SysEvent — firmware tearing down our widget |

**The big takeaway:** when the foreground is the firmware's own
**dashboard list** (the menu you scroll through), gestures collapse
into "List_ItemEvent navigation events" (`SCROLL_TOP` for tap-class,
`SCROLL_BOTTOM` for scroll-down). When the foreground is **our hijacked
list widget**, gestures arrive as `List_ItemEvent CLICK` against our
container (this is what `handleHijackMenuTap` consumes). When the
foreground is a **TEXT widget**, gestures arrive as `SysEvent` carrying
the actual event type — `DOUBLE_CLICK_EVENT(3)` for ring double-tap,
not the conflated SCROLL_TOP we'd see on the dashboard. So the same
physical gesture produces different event values depending on widget
context.

### Critical: events are firmware-filtered

The firmware **only emits cmd=3 when navigation actually changes UI
state**. Specifically:

- Scroll up at the top of the dashboard → no scroll happens → **no event**.
- Scroll down past the last item → no scroll happens → **no event**.
- Double-tap when display is already on and at top → still fires `SCROLL_TOP`
  (the firmware's "tap = nav-back / select / scroll-top" semantics).
- Tap-and-hold on the ring → fires nothing on sid=0x01 cmd=3, only
  `sid=0x0D USER_ACTIVITY 8B src=3`.

**You cannot use this channel for raw gesture detection.** It reports
"the dashboard widget's UI changed because of input," not "the user
performed gesture X." Raw input would have to come from somewhere else
— possibly the not-yet-explored sid=0x80 keepalive channel, or one of
the unused 7450/1001 services if those ever start emitting on a future
firmware.

### Pairing with sid=0x0D follow-ups

Tap-class gestures **on the dashboard list** (which encode as
`SCROLL_TOP`) are also followed ~270 ms later by a `sid=0x0D
USER_ACTIVITY` 6-byte event, because the firmware additionally wakes
the display state machine. Pure scroll gestures don't wake the display,
so no sid=0x0D follow-up. So the practical decoder for **dashboard**
events is:

- `cmd=3 EventType=2` → user scrolled down (advanced into a panel)
- `cmd=3 EventType=1` **with** sid=0x0D USER_ACTIVITY 6B within 500 ms → tap-class gesture
- `cmd=3 EventType=1` **without** sid=0x0D follow-up → user scrolled up
- `sid=0x0D USER_ACTIVITY 8B src=3` with no preceding cmd=3 → glasses-temple long-press
- `cmd=3` with **inner `codes={4,5}`** + `sid=0x0D USER_ACTIVITY 8B src=4` → **notification widget interaction** (clicking through a stacked notification, dismissing). Observed 2026-04-26. The `{4,5}` pair is distinct from the `{0,1}` / `{1,2}` pairs that fire on regular dashboard navigation; treat as notification-app-specific.

This pairing pattern **does NOT apply to TEXT widgets**. While a TEXT
widget is foreground, the firmware sends gesture events as
`sid=0xE0 SysEvent` carrying the actual event type and source, and
**no sid=0x0D follow-up fires at all**. Verified against firmware
2.2.0.242: ring double-tap on a TEXT widget produces a single
`SysEvent.DOUBLE_CLICK(3) src=2` frame and nothing else. So if you're
listening for "user did something" while a TEXT widget is up, listen
on sid=0xE0 SysEvent, not sid=0x0D.

### Channel topology — ring lives on the right temple

Every cmd=3 event observed against this firmware arrived on the
**right** temple's notify pipe. The Even app's pairing model puts the
ring↔glasses pairing inside the glasses' own firmware; once paired,
the glasses become the BLE relay for ring events, and 2.2.0.242 sends
them on the right temple specifically. If a cmd=3 frame ever shows up
on the LEFT temple, that's either a fallback channel or a firmware
behaviour change worth investigating — the dispatcher in
`Optional_EvenG2.cpp` logs a `[G2] !!! GESTURE on LEFT temple` banner
plus a frame ring-dump if it ever happens.

### Notify channel topology on firmware 2.2.0.24 — right-only

Broader observation against firmware **2.2.0.24** (captured 2026-04-27):
the right-temple-only routing isn't just for cmd=3 / ring events — it
covers **the entire notify channel**. Across a session of normal use,
a steady-state `tx/rx` snapshot looked like:

```
Left :  rx 0       (zero notifies, ever)
Right:  rx ~900+   (everything)
```

Notifies seen exclusively on the right temple in 2.2.0.24:
- sid=0x01 events (cmd=3 gestures, notification-stack countdowns, scroll cmd=11/12)
- sid=0x0D USER_ACTIVITY events
- sid=0x80 keepalive pings
- sid=0x09 battery / settings async pushes
- sid=0xE0 image-push acks (cmd=4)
- Firmware heartbeat replies

**This conflicts with the older 2.2.0.242 doc claim that sid=0x0D
"both arms emit the same event simultaneously."** On 2.2.0.24 the left
arm emits nothing — caller-side dedupe is unnecessary because there's
no duplicate to dedupe.

**Practical consequences:**
- Cached battery for the left arm stays unknown (`?%`) — we never see
  the sid=0x09 push there.
- TX is symmetric for heartbeats (we still send to both arms) but
  asymmetric for image pushes (currently right-only by design); RX is
  right-only by firmware behaviour.
- If you want a "is the connection healthy on the left arm" signal,
  you can't rely on inbound traffic; use only the heartbeat ack
  round-trip on the left's own write characteristic instead.

If a future firmware revision restores dual-arm notify, the existing
left-arm dispatcher path will start receiving frames again — keep it
wired even though it's quiet today.

### Counter streams

A single monotonic counter is used for *all* cmd=3 events on this
channel — input gestures (`field 5` body) and notification-widget
state updates (`field 3` body) share the same sequence and just
toggle which body field is populated. Counter resets to ~0 on plugin
restart. Earlier docs called the `field 3` family "idle beacons / ignore
for input handling" — that's wrong, see the next section.

### sid=0x01 cmd=3 field 3 — notification-stack countdown

When the user opens the firmware's built-in **notification panel** and
single-taps to dismiss notifications, cmd=3 frames arrive carrying
`field 3` (instead of `field 5`) inside the `field 6` body. The inner
shape encodes the remaining notification count, decrementing per tap.
Captured 2026-04-27 against firmware 2.2.0.24 — 9 stacked notifications
dismissed one by one.

**Per-tap frame** (counter monotonic, inner count decrements):

```
32 0B 08 <ctr varint> 1A 06 0A 02 08 <N> 12 00
│  │   │              │  │  │  │  │  │   │  │
└── f6 (body) len=11
   ├── f1 = counter (e.g. 2081)
   └── f3 = 6 bytes nested:
       ├── f1 (len-delim, 2 B) = { f1 = N }   ← remaining-count
       └── f2 (len-delim, 0 B) = empty
```

Captured countdown across one drain (counter 2081 → 2090):

| counter | inner f1 (count) | meaning |
|---|---|---|
| 2081 | 9 | panel opened, 9 notifications stacked |
| 2082 | 8 | tap-dismiss |
| 2083-2089 | 7..1 | tap-dismiss x7 |
| 2090 | **(shape changes)** | terminal — see below |

**Terminal "stack drained" frame** has a distinctive shorter shape
where the inner f1 is replaced by an *empty* length-delim:

```
32 09 08 <ctr> 1A 04 0A 00 12 00
              │  │  │  │  │  │
              └── f3 = 4 bytes:
                  ├── f1 (len-delim, 0 B) = empty   ← signature: count cleared
                  └── f2 (len-delim, 0 B) = empty
```

**Detection rules:**
- `f6.f3 = { f1 = { f1 = N }, f2 = empty }` → notification panel is
  open, N notifications remaining.
- `f6.f3 = { f1 = empty, f2 = empty }` → user just cleared the last
  one (or panel closed with stack already empty). Use this as the
  "notifications drained" trigger if you ever want to react.
- `f6.f5 = List_ItemEvent` → ordinary dashboard nav (the existing
  documented case).

**Side note on user-activity pairing:** the corresponding
`sid=0x0D USER_ACTIVITY src=4` events fire only at the *open* of the
notification panel (1-3 frames typically), **not** per dismiss. Per-tap
cadence rides entirely on cmd=3 `f3`. So count notification-dismisses
on the cmd=3 channel, not sid=0x0D.

### List_ItemEvent inner f1 ≠ 1 (panel-open / drawer-open variants)

Originally documented as "always 1." Empirically (firmware 2.2.0.24,
captured 2026-04-27 against the built-in Health panel and Quicklist),
the inner `field 1` of List_ItemEvent takes other small varint values
that flag *what kind of UI surface* opened, with the `field 2` nested
sub-message reshaped accordingly. Logged by the parser as
`async cmd=3 ... flag=N (no codes)` when N ≠ 1.

| f1 | Meaning | Companion shape |
|---|---|---|
| 1 | Regular dashboard list nav | `f2 = { CurrentSelectItemIndex, EventType }` (the documented case) |
| 5 | **Quicklist / drawer opened** | `f6 = nested` (longer payload, content not yet decoded) |
| 6 | **Detail panel entered** (e.g. tap into Health) | `f7 = empty bytes` — no item index, just "panel up" |

The companion `sid=0x0D USER_ACTIVITY` follow-up still fires for these,
so they pair the same way as f1=1 tap-class events for "user did
something." Treat f1=5/6 as breadcrumbs of *which* firmware-owned
surface is now foreground; the actual content of the panel (Health
metrics, Quicklist items) is rendered entirely on-glass and never
appears on the wire.

### sid=0x01 cmd=11 / cmd=12 — built-in app scroll/select stream

A second message family on `sid=0x01 flag=0x01`, distinct from the
cmd=3 dashboard channel. Discovered 2026-04-27 while scrolling through
the built-in News reader; the same shape will likely appear in any
other firmware-owned scrollable app (Quicklist, etc.).

**Wire shape** (no timestamp, no `field 6` envelope — flat payload):

```
sid=0x01 flag=0x01 cmd=11   → field 1 = 11
                              field 2 = 1                (small varint, role unclear)
                              field 13 = nested body     (tag 0x6a, len-delim)
                                field 1 = varint         (selection / index)

sid=0x01 flag=0x01 cmd=12   → field 1 = 12
                              field 2 = 1
                              field 14 = nested body     (tag 0x72, len-delim — empty
                                                          when scroll has no payload)
                                field 1 = varint         (event sub-kind: 2 = scroll)
                                field 2 = varint         (scroll index, monotonic)
```

Captured sequence while scrolling News (each line one notify, ~250-1200 ms apart):

```
cmd=12 f14=empty                             (entered news view)
cmd=12 f14={f1=3}                            (initial position?)
cmd=12 f14={f1=2, f2=1}   ← scroll tick 1
cmd=12 f14={f1=2, f2=2}   ← scroll tick 2
cmd=12 f14={f1=2, f2=3}   ← scroll tick 3
cmd=12 f14={f1=2, f2=4}   ← scroll tick 4
cmd=11 f13={f1=1}                            (exit / select?)
```

**Practical takeaway:** if we ever push our own list-shaped widget to
the firmware's built-in surface (vs. our hijacked container), this is
the channel that would tell us which row the user is on. The cmd=12
field 14 sub-fields (`{f1=2, f2=index}`) make scroll position decodable
without any application-side state.

**Not yet decoded:**
- The leading `field 2 = 1` constant — could be a session/app id.
- Whether cmd=11 and cmd=12 are list-specific or fire for other
  firmware apps (calendar, settings, etc.).
- What `f14={f1=3}` means at view-entry vs. `f14={f1=2,...}` during scroll.

### Quicklist check-offs are silent

When the user toggles items inside the built-in Quicklist (checking /
unchecking entries), **zero BLE traffic flows** — neither sid=0x01 nor
sid=0x09 emits anything. Confirmed 2026-04-27. The Quicklist's checked-
state is owned entirely by the glasses' on-board UI; the host never
sees it. Implication for any future "to-do" / list feature we host: do
not assume we can mirror the built-in Quicklist's state — we'd have to
ship our own widget to observe interaction.

### sid=0x09 battery push — unsolicited

The settings channel pushes battery on its own without the host having
to query. Captured 2026-04-27 mid-session: a `sid=0x09 flag=0x01`
notify arrived carrying the standard battery payload, parsed as
`Battery: 79% (async)`. Trigger conditions aren't fully characterised
(seems to fire on level-change crossings and possibly app-state
transitions), but the practical takeaway: **the host can passively
scrape battery without ever sending a query** — just listen on sid=0x09
and let the firmware push.

### Gesture flag taxonomy on cmd=3 (firmware 2.2.0.24)

Consolidating across captures, the parser's reported "flag=N" line for
`sid=0x01 cmd=3` actually concatenates two distinct fields. The
envelope `flag` is `0x01` (`flag=0x01`); the inner List_ItemEvent
`field 1` is what varies. Mapping observed so far:

| Inner f1 | Companion field | Surface |
|---|---|---|
| 1 | `f2 = { idx, EventType }` — the documented `codes={A,B}` form | Dashboard list nav — including the `{4,5}` notification-widget interaction |
| 5 | `f6 = nested` | Quicklist / drawer open |
| 6 | `f7 = empty` | Detail-panel enter (Health, etc.) |

The exit-detail-panel gesture (e.g. tap to leave Health) still arrives
as the standard `f1=1, codes={4,5}` form — so an "enter" reports
`f1=6` and the matching "exit" reports `f1=1, codes={4,5}`,
asymmetrically. Useful as a "user is currently inside firmware-owned
detail panel" hint if we ever want to suppress our own pushes while
the firmware foreground is busy.

## Audio (sid=0xE0 Cmd=15, render notify 6402)

Verified end-to-end on firmware **2.2.0.24** (2026-04-28): captured a
10 s sample via the `g2micrec` CLI, decoded with `liblc3`, audio is
intelligible speech.

**Start sequence:**

1. **A StartUpPage container must be active** on the lens before
   `AudioCtrCmd` will be honored. Without one, the firmware silently
   drops the request — no `AudioCtrRes` ack, no frames. Drilling into
   any G2 hijack page (e.g. `g2network`) creates the container.
   Confirmed empirically — same `AudioCtrCmd` produces zero frames
   when sent to a blank lens, and ~20 fps frames the moment a list
   page is up.
2. Host sends EvenCore `AudioCtrCmd { AudoFuncEn = 1 }` on sid=0xE0
   Cmd=15 (yes, `Audo` — sic).
3. Await `AudioCtrRes` (Cmd=16 magic=207) on sid=0xE0 — confirms the
   stream is enabled.
4. LC3 frames begin arriving on `6402` (service `6450`) of the **LEFT**
   temple. RIGHT's `6402` stays silent on this firmware (verified by
   subscribing both arms; only L emits).

**Packet layout (verified):** every notification on `6402` is exactly
**205 bytes**, arriving at ~20 packets/sec (50 ms of audio per packet):

```
offset 0..199 :  five LC3 frames × 40 bytes each
offset 200..204:  5-byte trailer (counter + status + padding)
```

LC3 codec parameters: **16 kHz mono, 10 ms frame duration, 32 kbps**.

*Empirical correction note:* both this doc (pre-2026-04-28) and
[g2-kit-unofficial/ble/audio.ts](https://github.com/Commute773/g2-kit-unofficial/blob/main/ble/audio.ts)
speculated the framing was "5 B header at start, then 5×40 B LC3". That
guess produced unintelligible audio. The actual layout has the LC3 data
**first**, with the metadata as a 5 B trailer. Discovered by per-column
entropy analysis across 209 captured packets — columns 200–203 showed
distinct low-entropy patterns while 0–199 looked like compressed audio:

| Trailer byte | Entropy | Dominant value(s) | Likely meaning |
|---|---|---|---|
| 200 | high  | varies | (within high-entropy region) |
| 201 | 0.41  | `0x00` (194/209) | counter / state, mostly zero |
| 202 | 2.26  | `0xF8` (111), `0x00` (35), `0xEF` (22) | flag/status |
| 203 | 0.73  | `0xFF` (166), `0x00` (43) | padding / mode bit |
| 204 | high  | varies | (semantics unconfirmed) |

Exact trailer semantics are unverified — for capture/decode purposes,
just discard the last 5 bytes.

**Other notes:**

- Each arm has its own mic per the kit; on 2.2.0.24 only LEFT actually
  emits over BLE.
- Mic dies with the plugin task — heartbeats are mandatory while
  capture is active.
- The stream auto-stops when the StartUpPage tears down (DISPLAY_OFF,
  hijack exit, etc.). Send `AudioCtrCmd { AudoFuncEn = 0 }` for a
  graceful stop.

**Tooling:** `g2micrec start [path]` / `g2micrec stop` dumps raw 205 B
packets to SD as a `.lc3` file. Decode offline with
[randomscripts/decode_g2_mic.py](randomscripts/decode_g2_mic.py) using
the `16k_10ms_5x40_5b_tail` layout.

## Front pane: Even-AI overlay (sid=0x07)

The G2 has **two physical depth planes** (focal depths), not just two
z-layers on the same surface. The "Hey Even" voice popup and the AI
answer card live on the front (closer) plane; everything we drive via
EvenHub (sid=0xE0, the back/dashboard plane) lives on the further plane.
The front plane is the Even-AI subsystem: `UI_FOREGROUND_EVEN_AI_ID = 7`
in the firmware's `service_id_def_pb.ts`. Schema is `EvenAIDataPackage`
in `even_ai_pb.ts`. Verified host-driveable on hardware 2026-04-26.

### Wrapper schema

```
EvenAIDataPackage {
  uint32 commandId   = 1;   // eEvenAICommandId
  uint32 magicRandom = 2;
  EvenAIControl     ctrl       = 3;
  EvenAIVADInfo     vadInfo    = 4;
  EvenAIAskInfo     askInfo    = 5;
  EvenAIAnalyseInfo analyseInfo = 6;
  EvenAIReplyInfo   replyInfo  = 7;
  EvenAISkillInfo   skillInfo  = 8;
  EvenAIPromptInfo  promptInfo = 9;
  EvenAIEvent       event      = 10;
  EvenAIHeartbeat   heartbeat  = 11;
  EvenAICommRsp     resp       = 12;
  EvenAIConfig      config     = 13;
}
```

### `eEvenAICommandId`

| ID | Name       | Direction (verified) | Notes |
|----|------------|----------------------|-------|
| 0  | NONE_COMMAND | — | sentinel |
| 1  | CTRL       | bidir | host→glasses to enter (status=ENTER); firmware echoes status; wake-word fires from firmware as status=WAKE_UP |
| 2  | VAD_INFO   | glasses→host | voice activity detected |
| 3  | ASK        | host→glasses | populate the question panel |
| 4  | ANALYSE    | host→glasses | "thinking" transition |
| 5  | REPLY      | host→glasses | answer card body — supports streaming |
| 6  | SKILL      | host→glasses | invoke built-in skill (BRIGHTNESS, NAVIGATE, etc. — unverified) |
| 7  | PROMPT     | host→glasses | error prompts (NETWORK_ERR / TROUBLE_UNDERSTAND / etc.) |
| 8  | EVENT      | glasses→host | SCROLL / STREAM_COMPLETE |
| 9  | HEARTBEAT  | host→glasses | keep-alive (unverified) |
| 10 | CONFIG     | host→glasses | voiceSwitch / streamSpeed / duplexMode (unverified) |
| 12 | COMM_RSP   | glasses→host | error-code ack |

### `eEvenAIStatus` (EvenAIControl.status)

| Value | Name | Trigger |
|-------|------|---------|
| 0 | STATUS_UNKNOWN | sentinel |
| 1 | EVEN_AI_WAKE_UP | wake-word activated; firmware-initiated |
| 2 | EVEN_AI_ENTER | "open the AI app" — sent by us to bring the front pane up |
| 3 | EVEN_AI_EXIT | dismiss / auto-timeout |

### Verified host-driven pipeline

```
1. CTRL{status=ENTER}        — opens the front-pane app (lands in LISTEN)
2. ASK{text=<heading>}       — populates the question panel; transitions out of LISTEN
3. ANALYSE                   — "thinking" intermediate state
4. REPLY{text=<body>, fTextEnd=1}  — paints the answer card
```

Each step needs ~200 ms settle. Each carries its own `magicRandom` so
the firmware's ack-correlation logic doesn't collapse them. The
firmware acks every step on flag=0x00 and emits an `EVENT
{type=STREAM_COMPLETE}` ~800 ms after the final REPLY.

The firmware paints the question panel and the answer card stacked on
the same plane simultaneously. ASK text effectively acts as a heading
above the body — useful when you want a `Title | Body` layout. Empty or
whitespace-only ASK text might be treated as "no question," which keeps
the FSM in LISTEN; pass at least a placeholder like `"(host)"` if you
don't need a real heading.

### Ways the pipeline can fail visibly

- **CTRL{ENTER} then REPLY** (no ASK / no ANALYSE): the firmware accepts
  every frame at the protocol layer (ack with errorCode=0) but the lens
  shows only the listening UI and ignores the REPLY content.
  Empirically the FSM has to transition through QUESTION→ANALYSING
  before REPLY paints anything. This is why the original CTRL+REPLY
  approach was a dead end.
- **"Hey Even" wake-word path** uses a different control flow — firmware
  fires CTRL{WAKE_UP} itself, then expects ASR-driven ASK. Sending
  ENTER on top of an active wake-word session has not been tested.
- **CTRL{ENTER} returns errorCode=7 from some hijack contexts.**
  Observed 2026-04-26: invoking the No-ASK pipeline from the Network
  sub-page's Scan worker returned `COMM_RSP errorCode=7` to the CTRL
  ENTER, the front-pane card never opened, and ANALYSE/REPLY still
  acked but rendered nothing. The exact same code path called from the
  Test Suite's AI Panel Tests sub-menu (`spawnAIWorker(AIWK_LOADING)`)
  succeeded, with `CTRL status=ENTER` ack, `SysEvent FG_ENTER`, full
  pipeline rendering, and clean `EVENT STREAM_COMPLETE` + `CTRL EXIT`
  on dismissal. Both invocations: same wrapper, same magic, same
  worker pattern. The difference is firmware state — possibly the
  rate at which the firmware accepts back-to-back app transitions or
  some app-context bookkeeping we don't see. Until characterised,
  treat the front-pane card as best-effort from any non-Test-menu
  context and have a back-pane fallback (Network scan now uses one).
  Test menu invocation is reliable for iterating on the front-pane UX.

### Auto-dismiss

The firmware auto-dismisses the answer card ~10 s after the final
REPLY, emitting `CTRL{status=EXIT}`. `EvenAIConfig` (cmd=10) has
`voiceSwitch / streamSpeed / duplexMode` knobs but **no documented
display-time field**, so the auto-dismiss window is not host-tunable
from this API.

### Calling from the BLE notify task — known deadlock

The four-step pipeline does sequential `sendEnvelope` calls with
`vTaskDelay`s between. Calling it directly from a BLE notify task (e.g.
a tap dispatcher) **wedges the right temple's writeMutex permanently**:
the first envelope leaves, then the BT stack's reentrancy on the same
task prevents `xSemaphoreGive` from firing. Subsequent TX from any
context (heartbeats, `g2reopen`, etc.) returns `Write mutex timeout`,
and the firmware eventually shows "Connection lost". Only a reboot
recovers.

The fix is the same worker-task pattern the page-swap path uses
(`sendCreateAndWait` / `sendShutdownAndSettle` are explicit about this):
spawn a one-shot task, return from the dispatcher within microseconds.
See `G2_Page_TestSuite.cpp:spawnAIWorker()` for the pattern.

### Notes on `streamSpeed` / streaming REPLY

We single-shot REPLY today (`fTextEnd=1` on the only chunk). Streaming
is supported by the schema (`cmdCnt` increments per chunk, `fTextEnd=0`
on intermediate chunks, `fTextEnd=1` on the last). Useful if we ever
exceed the single-fragment cap (~230 B of text after pb overhead).

## Other firmware-defined surfaces

The firmware enum lists many more per-app sids than we drive. All of
these have schemas in the reference's `*_pb.ts` but no working examples
in g2-kit-unofficial — i.e. the wire shape is defined but behaviour is
empirical. Cataloged here so we don't keep rediscovering them. Status
column reflects probe results from this codebase (2026-04-26).

| sid | Name | Schema | Status |
|-----|------|--------|--------|
| 0x05 | Translate | `translate_pb.ts` — TranslateDataPackage with TRANSLATE_RESULT carrying srcText/dstText | **reachable.** `g2probe 05 1` (empty CTRL) gets `COMM_RSP commandId=162 errorCode=7`. Proper body required to render. |
| 0x06 | Teleprompt | `teleprompt_pb.ts` — TelepromptDataPackage with PAGE_DATA carrying scrollable pages | **reachable.** `g2probe 06 1` (empty CTRL) gets `COMM_RSP commandId=166 errorCode=1`. Needs a `TelepromptControl{cmd=START, startSettings={...}}` body — i-soxi/even-g2-protocol has a working transcript. |
| 0x08 | Navigation | `navigation_pb.ts` — basic_info_msg with directionSign/distance/roadName/etaTime/spendTime/speed | observed firmware emits `cmd=1 LOCATION_LIST_REQUEST` to host on user "Navigate" tap; we don't respond. |
| 0x0A | Transcribe | `transcribe_pb.ts` — TranscribeResult with text/endFlag/speaker | **silent.** `g2probe 0A 1` gets no response. Either subsystem isn't initialized at runtime, requires a preconfig step, or doesn't exist on this firmware. |
| 0x0B | Conversate | `conversate_pb.ts` — TRANSCRIBE_DATA + tag bubbles (TagType KNOWLEDGE/QUESTION/PEOPLEWIKI/SUGGEST) | **reachable, FSM-gated.** `g2probe 0B 1` opens the Conversate app and acks with `COMM_RSP commandId=162 (empty body)`. Firmware then emits `PREP_NOTE_LIST_REQUEST (cmd=2)` asking the host for preparation notes. The visible UI is parked at "pick a note" — without a `PREP_NOTE_LIST (cmd=3)` reply from us, the user must tap to advance. State-event `state-6B code=11` fires on activation (matches `sid=0x0B`). |
| 0x0E | Health / WidgetXform | `health_pb.ts` (closed schema: steps/calories/sleep/HR/SpO2/temp/HRV/productivity) | unverified — also conflicts with our earlier "widget transform" guess; needs labelled capture. |
| 0x21 | ForegroundSystemAlert | no `*_pb.ts` in reference | unknown — schema not exposed. |
| 0x81 | GlassesCase | undocumented | **observed glasses-emitted.** When the charging case is around, the firmware emits `[08 01 10 <varint> 1A 06 08 0C 10 01]` shaped frames — likely case open/close + status. Host→case TX not yet attempted. |
| 0xC4/0xC5 | FileService cmd / raw | `efs_transmit_pb.ts` — ANCS-style notification body shipped as `ANDROID_MSG_JSON_NOTIFICATION` JSON file | unverified; JSON shape not documented; needs phone-app capture. |
| 0x80 | DeviceSettings | undocumented | **brick risk** — reference's `gotchas.md` records non-terminally bricking a pair via this sid; `g2probe` blocks it. |

### Conversate FSM (sid=0x0B)

Empty CTRL is *not* enough to fully activate Conversate, even though the
firmware acks. The FSM expects a preparation-note list:

```
host → CTRL=1 (CONTROL with empty body)
glasses → COMM_RSP=162 (success ack, empty body)
glasses → PREP_NOTE_LIST_REQUEST=2  ← firmware asks: "give me your note titles"
host → PREP_NOTE_LIST=3 (cmd=3, list of strings)         [WE DON'T DO THIS YET]
glasses → renders note picker on lens
user picks → PREP_NOTE_SELECT=4 (cmd=4, body={f1=index})
glasses → "Starting conversate" text appears, FSM enters live mode
host → TRANSCRIBE_DATA=6 / TAG_DATA=5 to populate the live overlay
```

Without step 3, the firmware sits in "pick a note" mode with an empty
list. Empirically the user can tap the ring/glasses to bypass — that
fires a synthetic `PREP_NOTE_SELECT(index=1)` and the FSM advances
even with no notes registered. So Conversate IS host-driveable, but
properly requires the host to handle the `PREP_NOTE_LIST_REQUEST`
callback rather than just firing CTRL and walking away.

For a fully host-driven Conversate session, we'd need to:

1. Watch for `cmd=2 PREP_NOTE_LIST_REQUEST` after our CTRL=1
2. Reply with `cmd=3 PREP_NOTE_LIST` (one or more synthetic note titles)
3. Either auto-fire `cmd=4 PREP_NOTE_SELECT` ourselves, or wait for the
   user to tap
4. Push `cmd=6 TRANSCRIBE_DATA` / `cmd=5 TAG_DATA` for live content

That's a real driver, not a probe — out of scope for `g2probe` but
plausible as a future feature on top.

To probe sids without writing C, use the `g2probe` CLI (see
"Protocol exploration tooling" below).

## Firmware font coverage (empirical, firmware 2.2.0.24)

The G2's lens font is **partial Unicode** — coverage is per-codepoint
rather than per-block, so neighbouring glyphs in the same Unicode range
don't necessarily all work. Tested 2026-04-26 against firmware version
`2.2.0.24` via the on-glasses Character Tests sub-menu (rendered through
`g2ShowListPage` on the hijacked widget). Re-test against newer firmware
revisions before relying on edge-case glyphs.

### The horizontal-vs-vertical precision rule

The single most useful pattern that emerged from the second testing
pass: **the firmware's font supports glyphs that fill rows of the cell
but not glyphs that need column or quarter-cell precision.**

- Top-half (▀) and bottom-half (▄) render cleanly. Left-half (▌) is
  unreliable; right-half (▐) generally fails.
- Left-half-circle (◐) and right-half-circle (◑) render. Top-half (◓)
  and bottom-half (◒) miss.
- All 10 quadrant glyphs (▖▗▘▙▚▛▜▝▞▟) miss — quarter-cell precision is
  absent entirely.
- Up/down filled triangles (▲▼) render; left/right (◄►) miss.

Design implication: when picking glyphs for split panels, two-tone fills,
spinners, or direction indicators, prefer ones that fill horizontal
strips of a cell. Anything that asks the font to half-fill a cell
vertically is a coin toss.

### What renders cleanly (use these)

| Class | Codepoints | Glyphs | Notes |
|---|---|---|---|
| ASCII | U+0020 – U+007E | full standard ASCII | All printable ASCII |
| **Eighth-blocks** | U+2581 – U+2588 | `▁ ▂ ▃ ▄ ▅ ▆ ▇ █` | **All 8 levels** — best smooth progress / volume / signal bars |
| **Light box-drawing** | U+2500, U+2502, U+250C, U+2510, U+2514, U+2518, U+251C, U+2524, U+252C, U+2534, U+253C | `─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼` | Borders, tables, panel chrome |
| **Hatched squares** | U+25A4 – U+25A9 | `▤ ▥ ▦ ▧ ▨ ▩` | **All 6 distinct** — best non-density shading variety on this firmware |
| **Geometric shapes** | U+25A0/A1/AA/AB, U+25CF/CB, U+25C6/C7 | `■ □ ▪ ▫ ● ○ ◆ ◇` | Both filled and outline variants of square / circle / diamond |
| **Single arrows + bidir** | U+2190 – U+2195 | `← → ↑ ↓ ↔ ↕` | Nav indicators including bidirectional |
| Top/bottom half-blocks | U+2580, U+2584 | `▀ ▄` | Use for horizontal split panels; combine for two-tone progress |
| Filled up/down triangles | U+25B2, U+25BC | `▲ ▼` | Direction indicators (vertical) |
| Left/right half-circles | U+25D0, U+25D1 | `◐ ◑` | 2-frame "pulse" spinner |
| Bullets / stars (subset) | U+2022, U+25E6, U+25AA, U+25AB, U+2605, U+2606 | `• ◦ ▪ ▫ ★ ☆` | Bullet variants + filled / outline stars |
| Math / units (subset) | U+00B1, U+00D7, U+00F7, U+00B0, U+2032/33, U+00B5, U+03A9, U+221E | `± × ÷ ° ′ ″ µ Ω ∞` | Sensor labels, units |
| Currency | U+00A2, U+00A3, U+00A5, U+20AC, U+20B9 | `¢ £ ¥ € ₹` | Common currency symbols |
| Solid block | U+2588 | `█` | Stack with spaces for ASCII-style progress |

### Partial coverage (some glyphs in the set render)

| Class | Codepoints | Renders | Misses |
|---|---|---|---|
| Shade blocks | U+2588/91/92/93 `█▓▒░` | `█` solid; `▓` as **diagonal stripes** (not dark fill) | `▒` and `░` blank |
| Half-blocks | U+2580/84/8C/90 `▀▄▌▐` | `▀ ▄` reliable; one of `▌▐` rendered on 2026-04-26 | the other vertical half |
| Triangles | U+25B2/BC/C4/BA `▲▼◄►` | `▲ ▼` | `◄ ►` (newer block) |
| Quarter-circle spinner | U+25D0–D3 `◐◓◑◒` | `◐ ◑` (left/right halves) | `◓ ◒` (top/bottom halves) |
| Double-line box | U+2550 – U+256C | `═` only | `║ ╔ ╗ ╚ ╝ ╠ ╣ ╦ ╩ ╬` |
| Double arrows | U+21D0 – U+21D3 `⇐⇒⇑⇓` | `⇒` only | other 3 |

### What fails (avoid; fall back to ASCII)

| Class | Codepoints | Fallback |
|---|---|---|
| Quadrants | U+2596 – U+259F (10 glyphs) | None — quarter-cell precision absent. Use eighth-blocks for vertical fills, hatched squares for textured fills. |
| Checks / Xs | U+2713/14/17/18 `✓ ✔ ✗ ✘` (Dingbats) | ASCII `[x]` / `[ ]` |
| Braille pattern | U+2800-block | ASCII spinner `\|/-\\|/-\\` (4-step) or `.oO0` |

### Whitespace handling

The firmware's text engine **collapses whitespace** in list-item rendering:

- Literal tab `\t` (U+0009) is **stripped entirely** — not rendered as
  spaces or anything visible.
- A single space immediately inside `[` or `]` brackets is **trimmed**:
  the source `space[ ] end` renders as `space[] end`.
- Multiple consecutive spaces between words probably collapse to one
  (not exhaustively tested).

Confirmed working: a single space between glyphs in a row IS preserved
(verified by the "Spacing: shades + spaces" test — `█ ▓ ▒ ░` rendered
with the spaces kept). The collapse only kicks in for runs of multiple
spaces and inside brackets.

If you need precise spacing — e.g. fixed-width column padding inside a
ListContainer item — use a non-space filler (period, underscore, or a
box-drawing horizontal `─`) rather than relying on multiple spaces.

### UI design rules of thumb

- **Progress / volume / level bars:** use the eighth-block series
  `▁▂▃▄▅▆▇█`. Smooth per-pixel-row fill density without needing shade
  glyphs.
- **Textured shading / fill variety:** hatched squares `▤▥▦▧▨▩` —
  six visually distinct fill patterns, all reliable.
- **Bordered panels / tables:** light box-drawing only (`─│┌┐└┘├┤┬┴┼`).
- **Loading spinner:** ASCII rotation (`|`, `/`, `-`, `\`) is the only
  fully-portable option. `◐ ◑` gives a 2-frame "pulse" if you want a
  Unicode look. Don't use Braille, quadrants, or quarter-circle's
  T/B frames.
- **Direction indicators:** single arrows `← → ↑ ↓ ↔ ↕` for all four
  directions plus bidirectional. For solid emphasis, `▲ ▼` work but
  `◄ ►` don't — use single arrows for left/right.
- **Two-tone / split panel:** horizontal split via `▀ ▄` works; vertical
  split via `▌ ▐` is unreliable (one usually renders, the other doesn't,
  and which one is which is firmware-specific).
- **Status indicators:** uppercase letters, `★` / `☆` for emphasis,
  `● / ○` for filled/empty. Avoid Dingbats checks/Xs.
- **Double-line emphasis:** unavailable. Use double-cell width (e.g.
  bracketed labels `[BORDER]`) instead.
- **Geometric callouts:** `■ □ ▪ ▫ ● ○ ◆ ◇` all render — useful for
  filled/outline pairs in legends or list bullets.

These findings live next to the `kUniBlocksTests[] / kUniArrowsTests[] /
kUniSymbolsTests[]` tables in `G2_Page_TestSuite.cpp` — extend the
tables to test new candidate glyphs and update this section after each
test run. Test entries are ordered working → partial → broken; the
"(broken)" suffix on a label flags re-test candidates.

## Protocol exploration tooling

These CLI commands sit on top of the wire layer and are how we
empirically discover behavior the reference doesn't document.

### `g2protostats [verbose]`

Dumps a per-sid table of TX/RX counts, last flag, last pb length, and
the first 8 bytes of the most recent RX payload. Add `verbose` for the
full static reference of known sids and the EvenAI / EvenHub command
ID enums.

Tracker is silent (in-memory counters only — no log spam); only emits
when the CLI command runs.

### `g2probe <sid_hex> <cmd_dec> [body_hex]`

Builds a minimal `EvenHub`-style wrapper (`08 <cmd> 10 <magic> [body]`)
and sends it on the requested sid with flag=0x20. Use to fire a single
command without writing C. Inspect logs for the response.

```
g2probe 07 9                  # EvenAI HEARTBEAT
g2probe 07 10 080110A001       # EvenAI CONFIG voiceSwitch=0,streamSpeed=160
g2probe 0A 1                  # Transcribe CTRL — does the firmware ack?
```

`sid=0x80` is in a hardcoded blocklist (see DeviceSettings note above).

### `g2imgprobe [size_bytes]`

Sends a Cmd=3 multi-fragment image-data payload on sid=0xE0 with no
preceding CREATE-image. Used to verify the wire path and reassembler
work before we reverse `ImageContainerProperty`.

Verified 2026-04-28 against firmware 2.2.0.24: a 1054 B body fragments
into 5 BLE writes, the firmware reassembles them, and replies
`ImageRawResp` `cmd=4 magic=210` with body
`{containerId=1, name="img", w=1024, h=1024, errorCode=5}` —
**`errorCode=5` is `ImgRawFailed`**, the expected outcome when no image
container exists. Confirms:

- Multi-fragment Cmd=3 reassembly works end-to-end.
- The firmware-side response shape uses field 6 (`length-delim`) for an
  inner status sub-message with `errorCode` at field 8.
- No write-mutex timeouts at this size; 4 KB soft cap from
  g2-kit-unofficial gotchas still applies for larger probes.

### `g2aiconfig [voiceSwitch] [streamSpeed]`

Sends a Cmd=10 EvenAI CONFIG message on sid=0x07. With both args
omitted, ships an empty body (no fields set).

Verified 2026-04-28: empty-body CONFIG is accepted — firmware replies
on sid=0x07 with `cmd=10 magic=212` and no `errorCode` in the response
body, meaning the message parsed cleanly. Use this command iteratively
to learn the schema: vary field numbers/values and watch for an
`errorCode=1` response that flags a guess as wrong.

### `g2ai <text>` / `g2aih <heading>|<body>`

`g2ai` runs the verified CTRL→ASK→ANALYSE→REPLY pipeline with a
default `(host)` heading. `g2aih` lets you override the heading text;
the firmware paints heading + body simultaneously on the front pane.

Variants kept for empirical comparison: `g2ai-noask` (skip ASK,
hypothesis was that ANALYSE alone might suffice — unverified) and
`g2ai-direct` (CTRL+REPLY only — the original failing path, kept as a
control).

## Session recipe

For a fresh session that shows text on the lenses, the minimum correct flow
observed to work:

1. Scan, match both temples by name regex, copy the advertised device.
2. Connect to both peripherals.
3. Set ATT MTU to 244 (requested).
4. On each, get service `...5450`, get write+notify chars, subscribe to
   notify.
5. Send the 27-byte AppLaunch literal to each arm.
6. Wait 800 ms.
7. Start a 5-second heartbeat to **both** arms.
8. Send EvenCore Cmd=0 `CREATE_STARTUP_PAGE` with a single-item ListObject
   (the text becomes the one `ItemName`) — **to the right arm only**.
   Magic = 201.
9. Expect an ACK on sid=0xE0 flag=0x00 with Cmd=1 echoing your magic, inner
   `ResponseCreateStartupCmd.ResCmdMsg` = 0 (CreatePageSuccess). **Block on
   this ack** — the reference is explicit: first CREATE must ack before any
   subsequent page op will land. Later CREATEs of the same name can be
   fire-and-forget.
10. For subsequent text edits (`g2show "new text"`), send EvenCore Cmd=5
    `APP_UPDATE_TEXT_DATA_PACKET` with a `TextContainerUpgrade` referencing
    the same `ContainerName` + `ContainerID`. Magic = 206. Flicker-free,
    fire-and-forget. Use Cmd=7 REBUILD only when you need to change the
    container's geometry or swap it between list/text/image shape.
11. The lenses display the text. The display must be **awake** (double-tap
    to wake) for you to see it.

**Common pitfall:** sending REBUILD (Cmd=7) against a container that was
never CREATEd returns `RebuildResCmd.ResCmdMsg = 7 (RebuildFailed)` with
no visible effect on the glasses. This is what "silent text render" bugs
usually look like.

**Container lifetime:** the firmware tears down the StartUp container
the moment the display blanks. A 4-byte `DISPLAY_OFF` event on sid=0x0D
(payload `08 01 1A 00`) is the firmware's announcement that this has
happened. If the host keeps sending Cmd=5 `UPDATE_TEXT` against a
container whose display has since turned off, the firmware drops every
update silently — no ack, no ResCmd. The host **must** invalidate its
cached container state on every DISPLAY_OFF and re-issue CREATE before
the next render op.

**Cmd=17 menu-startup hijack:** when the user selects a built-in
mini-app from the glasses' menu, the firmware emits `sid=0xE0 cmd=17
OS_NOTIFY_MENU_STARTUP_PACKET` (wrapper field 20 → `MenuStartUpEvent`,
widgetId at inner field 1) and enters a "menu launch pending" state
waiting for the host's response. Sending Cmd=9 ShutDown at this point
fails with `res=11 APP_REQUEST_UPGRADE_SHUTDOWN_FAILED` because there
is no container to shut down yet — the firmware hasn't instantiated
one, it's waiting for us. The correct sequence:

1. Parse cmd=17, extract widgetId.
2. Send **Cmd=18 `APP_RESPONSE_MENU_STARTUP_FAILED_PACKET`** with any
   nonzero errorCode to cancel the pending launch. Wrapper field 21
   (`MenuStartUpResPonse {errorCode=1, errorString=2}`). Fire-and-forget —
   the reference schema defines no response for this packet.
3. Wait ~300 ms for firmware cleanup (observed empirically; too short
   and the CREATE below races the teardown).
4. Clear any locally-cached `containerReady` for that arm.
5. Send Cmd=0 CREATE with `CreateStartUpPageContainer.widgetId` set to
   the widgetId from cmd=17. The firmware now accepts it as "the widget
   that just launched is now ready" and your content replaces the
   mini-app's splash on screen.

**Re-entering a previously-hijacked widget:** if the user taps the same
menu item again (e.g. Blocks, widgetId=10509) after a prior hijack
ended, the firmware re-announces with a fresh cmd=17 but the host may
still think its old hijack is active (e.g. if the firmware bailed with
SYSTEM_EXIT but we missed the DISPLAY_OFF event). Treat any fresh cmd=17
for the same widgetId as **definitive proof the prior hijack is gone**
and force-clear the local active flag before proceeding — the glasses
never re-announce a widget that's already running.

### Widget lifecycle events (Cmd=2 DevEvent `SysEvent`)

While our hijacked widget is on-lens, the firmware narrates its own UI
state transitions through `Cmd=2 OS_NOITY_EVENT_TO_APP_PACKET` frames
carrying `SysEvent` (wrapper field 13 → `DevEvent.SysEvent`, wire path
is sid=0xE0 flag=0x01). The `EventType` inside is from the reference's
`OsEventTypeList` enum. Observed values and meanings:

| EventType | Name | What actually triggers it |
|---|---|---|
| 4 | `FG_ENTER` | A firmware-owned UI overlay has appeared *on top of* our widget — most commonly the built-in **"Exit? NO / YES" confirmation dialog** that tap-and-hold invokes. Our widget is paused, not destroyed. |
| 5 | `FG_EXIT` | A previously-entered firmware UI overlay has dismissed. If the user chose NO in the exit dialog, `FG_EXIT` alone fires and our widget resumes; if they chose YES, `FG_EXIT` fires followed shortly by `SYSTEM_EXIT`. |
| 7 | `SYSTEM_EXIT` | Firmware is tearing our widget down for real. On-lens "Connection lost" message appears. Clear all local widget state (`containerReady=false`, `hijackActive=false`) on this event — every subsequent EvenCore op will be dropped until a fresh CREATE. |

**Tap-and-hold exit flow, complete** (labelled capture 2026-04-24,
~hijack-alive 9.2 s):

```
[tap and hold]
sid=0xE0 flag=0x01 cmd=2 SysEvent{EventType=4 FG_ENTER}    ← confirmation UI appears
sid=0x0D pb=9 {code=224, 0x22}                              ← firmware UI transition marker

[user selects NO]
sid=0x0D pb=7 {code=224}                                    ← firmware UI transition marker
sid=0xE0 flag=0x01 cmd=2 SysEvent{EventType=5 FG_EXIT}      ← confirm UI dismissed
                                                             — widget resumes, no SYSTEM_EXIT

[user selects YES]
sid=0x0D pb=7 {code=224}
sid=0xE0 flag=0x01 cmd=2 SysEvent{EventType=5 FG_EXIT}      ← confirm UI dismissed
sid=0xE0 flag=0x01 cmd=2 SysEvent{EventType=7 SYSTEM_EXIT}  ← widget actually dies
sid=0x0D pb=4 {display-off}
```

The 7-byte `code=224` sid=0x0D frames are firmware-sub-UI markers (see
the sid=0x0D table above) — they cluster around FG_ENTER/FG_EXIT and
don't represent user input. Treat them as correlation signal for the
state transitions on sid=0xE0, not as actionable events themselves.

## Things that crash or wedge the glasses

Cataloguing what to avoid, from bitter experience:

- **Wrong envelope header size.** Sending an 11-byte header when the
  firmware expects 8 causes the pb parser to land 3 bytes into what it
  thinks is the payload; on some payloads this reboots the glasses.
- **Sending `CreateStartUpPage` inside the prelude.** The reference does
  not do this. Doing it before the plugin task is ready can reboot the
  glasses.
- **Missing / late heartbeats.** After >10 s of silence the plugin task
  dies and subsequent EvenCore commands are silently dropped until
  reconnect.
- **Per-fragment incrementing seq.** The firmware reassembles by seq —
  incrementing per fragment causes orphan tail fragments and silent drop.
- **Writing to sid=0x80 `dev_config`.** The reference flags this as "can
  brick glasses" — stay away.
- **MagicRandom values above 0xFF.** Firmware only compares the low byte
  for ack matching. Keep magic in `1..=255` and cycle as u8.
- **Multi-step BLE pipelines from a notify-task tap dispatcher.** Calling
  any sequence of `sendEnvelope` + `vTaskDelay` + `sendEnvelope` from the
  BLE notify task that just dispatched a `ListEvent CLICK` deadlocks: the
  first envelope leaves but the BT stack's reentrancy on the same task
  prevents `xSemaphoreGive(writeMutex)` from completing. Every
  subsequent TX returns `Write mutex timeout`; firmware shows
  "Connection lost" within seconds; only a reboot recovers. **Spawn a
  one-shot worker task** (see `G2_Page_TestSuite.cpp:spawnAIWorker` or
  the existing page-swap worker) and return from the dispatcher within
  microseconds. The CLI / web paths run in `cmd_exec_task` and are
  unaffected.

## Build-time / stack notes (ESP-IDF specific)

Specific to this codebase's bring-up, not protocol-level:

- The codebase uses **Arduino's stock BLE library** (`components/arduino/
  libraries/BLE`), not NimBLE-Arduino. API signatures:
  - `setAdvertisedDeviceCallbacks(BLEAdvertisedDeviceCallbacks*, wantDup,
    shouldParse)` — **not** NimBLE's `setScanCallbacks`.
  - `onResult(BLEAdvertisedDevice)` by value — not `const *`.
  - `start(duration_seconds, ...)` — **seconds**, not ms.
  - `registerForNotify(cb)` — not `subscribe(true, cb)`.
  - `BLERemoteCharacteristic::writeValue(uint8_t*, size_t, bool)` takes
    non-const first argument; const_cast when calling from a const path.
- The BLE controller does NOT coexist well with simultaneous server mode
  and client scanning. Disable `BT auto-start` in the setup wizard before
  using G2 client mode, or `initG2Client` has to tear down the server
  (flaky) first.
- The scan blocks the caller if you use `start(duration)` without a
  callback. Use the callback form (`start(dur, cb, false)`) so the CLI
  command handler doesn't time out.
- Arduino BLE leaks ~30 KB of DRAM per server→client tear-down cycle.
  Mitigated by not running server and client in the same boot.

## Hijack page-swap lifecycle

Once we've hijacked a built-in mini-app (e.g. Blocks, widgetId=10509), the
host owns the widget slot and can repaint its contents in response to user
taps — drilling from a top-level menu into a Status/Sensors/System submenu
and back. The naïve approach is to send `Cmd=7 REBUILD_PAGE` against the
existing container. **That reliably crashes the EvenCore plugin task** on
the firmware in our captured pair (left & right): the REBUILD ack arrives
with `RebuildResCmd.ResCmdMsg = 7 (RebuildFailed)` once, and from that
moment forward every EvenCore op is silently dropped — heartbeats included
— until the BLE link is re-established. `pluginDead` becomes the only
honest local state.

**REBUILD-text on a freshly-CREATEd text widget also dies on this
firmware** (verified 2026-04-26). Sending `Cmd=7 REBUILD_PAGE` with a
fresh `TextContainerProperty` against a container that was just
CREATEd via Cmd=0 silently fails — the firmware emits no
`Cmd=8 RebuildResp` ack and a few seconds later the lens overlays
"Connection lost". Same wire shape that the comment in `g2ShowText`
claims is "documented full-replace" doesn't actually work for
TEXT-after-TEXT-CREATE on this firmware. Earlier comments suggesting
LIST-vs-TEXT was the difference are wrong. Use SHUTDOWN+CREATE for
TEXT page swaps too — that's what `G2_Page_Settings.cpp`'s
`renderCurrentJsonPage` does (always full handshake regardless of
isInitialCreate).

### SHUTDOWN+CREATE workaround

Empirically, the only reliable repaint sequence is:

1. Send `Cmd=9 ShutdownPage` (exitMode=0) against the live container. Mark
   `gOurShutdownAtMs = millis()` first — see "echo guards" below.
2. Wait **500 ms** for firmware-side cleanup. Shorter windows (50–200 ms)
   have been observed to race the teardown and cause the next CREATE to
   be rejected.
3. Send a fresh `Cmd=0 CREATE_STARTUP_PAGE` carrying the new ListObject
   items, with `widgetId` set to the same value we hijacked (e.g.
   10509 for Blocks). Same widgetId → firmware treats it as the same app
   relaunching with new content; identity preserved, container fresh.
4. Block on the CREATE ack (Cmd=1, `ResCmdMsg = 0 CreatePageSuccess`)
   before considering `containerReady` again.

We do not have an explicit ShutdownResp wait — we observed `Cmd=10
ShutdownResp` arrives within ~100 ms, but it sometimes carries
`res=10 (err)` while the teardown still completes correctly. The 500 ms
fixed delay covers both the response latency and the firmware-side
release of the widget slot.

### REBUILD-list fast path (corrects 2026-04-25 claim)

**Update 2026-04-27:** the earlier blanket warning above that `Cmd=7
REBUILD_PAGE` with a different item set "reliably crashes the firmware
plugin task" is **wrong on firmware 2.2.0.24** — disproven empirically
by exercising rapid item-count changes via the runtime-tunable
`g2listrebuild` toggle:

| Transition | RebuildResp | Outcome |
|---|---|---|
| 7 items → 2 items | res=6 (Success) | OK |
| 2 → 7 | res=6 | OK |
| 7 → 6 | res=6 | OK |
| 7 → 9 | res=6 | OK |
| 9 → 7 | res=6 | OK |
| 7 → 12 | res=6 | OK |
| 12 → 7 | res=6 | OK |

All in rapid succession, no firmware wedge, no plugin-task death. The
prior crash claim came from testing on firmware 2.2.0.242, which may
behave differently — but on 2.2.0.24, **REBUILD-list is the correct
fast path** for in-place item swaps:

- ~70 ms wire time vs ~700 ms for SHUTDOWN+CREATE (10× faster)
- Single Cmd=7 envelope instead of two-round-trip teardown+create
- **No flicker** — the lens never blanks between content swaps
- Caveat: the firmware does **not** preserve cursor/selection state
  across the rebuild; the highlighted row resets to row 0 every swap.
  No CREATE/REBUILD-side schema field exists to seed an initial select
  index — this is a hard firmware limitation (see `ListContainerProperty`
  schema in g2-kit-unofficial — fields 1-12 fully accounted for, none
  for selection state).

The `g2listrebuild` CLI toggle defaults ON; it's exposed as a runtime
kill-switch in case a future firmware regresses. Runtime helper:
`sendRebuildListAndWait()` in `Optional_EvenG2.cpp` mirrors the CREATE
ack path with its own semaphore (`gRebuildAckSem`) keyed on
`G2_MAGIC_REBUILD = 202`.

### Live-list page primitive

Built on the REBUILD-list fast path: a page can opt into automatic
periodic refresh by setting `liveIntervalMs > 0` on its
`G2PageModule`. The dispatcher then renders via `g2StartLiveListPage()`
instead of `g2ShowTextAsList()` — an initial CREATE seeds the widget,
and a worker task ticks every `liveIntervalMs` calling the page's
`buildText` callback and shipping a Cmd=7 REBUILD with the fresh rows.

**Manual refresh trigger:** `SysEvent type=DOUBLE_CLICK(3) src=2` on
sid=0xE0 fires when the user double-taps a List widget — distinct from
the single-tap `ListEvent CLICK(0)` channel (corrects an earlier note
that "List widgets collapse double-tap to scroll-top"; that was true
for the firmware-owned dashboard list on 2.2.0.242, false for the
hijacked list on 2.2.0.24). The live-page worker watches for this event
and kicks an immediate refresh.

**Auto-cancel on navigation:** the page-swap worker calls
`g2StopLiveListPage()` at entry, so any tap that drills out of the live
page cleanly cancels the worker before the new content is rendered. No
race between two REBUILD streams targeting the same widget.

**First user:** the Status page (`liveIntervalMs = 5000`).

### Mixed-widget composition (List + Image in one CREATE)

`CreateStartUpPageContainer` is schema-defined as a multi-widget container —
`f1=ContainerTotalNum`, then *any combination* of `f2=ListObject`,
`f3=TextObject`, `f4=ImageObject` (each repeated). Until 2026-04-27 we'd
only ever shipped CREATE envelopes containing a single widget type;
empirical sweep that day (probes Q16/Q17/Q18 in
[Optional_EvenG2.cpp](components/hardwareone/Optional_EvenG2.cpp))
confirmed the firmware accepts mixed declarations on **firmware 2.2.0.24**:

| Probe | Geometry | Result |
|---|---|---|
| Q16 | List 8,8,560,130 + Image 144,144,288×144 (no overlap) | CREATE acked, both widgets render |
| Q17 | List 8,8,560,272 + Image 280,72,288×144 (overlap, right half) | CREATE acked, **image paints on top** |
| Q18 | List 8,8,560,272 + Image 488,8,80×80 (corner icon) | CREATE acked, **non-288×144 image OK** |

Findings:

- **One CREATE, multiple widget types** — no need to issue separate
  envelopes per widget. Build `f1=N`, then emit one `f2`/`f3`/`f4`
  per widget; firmware sets them up atomically.
- **Z-order: image > list.** When containers overlap, the image widget
  paints on top of the list widget. Useful for icon overlays on a
  text menu, less useful if you want the list visible underneath —
  position the image so it doesn't cover rows you care about.
- **Image containers can be smaller than 288×144.** The "BMP must
  match container size" rule from Q5/Q7 still applies (the BMP's
  width/height must equal the container's `w`/`h`), but containers
  themselves are free-form — Q18 ran a 80×80 tile cleanly. The 4-bpp
  row-stride alignment requirement (`(w+1)/2` rounded up to a 4-byte
  multiple) still applies to the BMP body.
- **Single CREATE budget:** the runtime helper
  `g2BuildCreateMixedListImage` builds into a 1 KB envelope buffer; a
  ~5-row list plus one image fits comfortably. Larger lists (≥10 rows
  with long strings) may exceed the budget — split into separate
  CREATE+CREATE if needed (untested).

Event channels are still per-widget — single-tap on the embedded list
arrives as `ListEvent CLICK(0)` on the list's container name; the image
widget itself emits no events. (See note on probe-hold dismissal:
`Optional_EvenG2.cpp` watches both `SysEvent DOUBLE_CLICK(3) src=2` and
`ListEvent CLICK(0)` to dismiss image probes that surface a list,
because users expect single-tap exit on either gesture.)

### Worker-task pattern

`g2ShowListPage` must not run on the BLE notify task. CREATE-ack
correlation uses a FreeRTOS semaphore that the notify task signals; if
the notify task is the one waiting on it, the system deadlocks. The
public entry point spawns a `g2_page_swap` worker (4 KB stack,
`tskIDLE_PRIORITY + 2`), deep-copies the caller's item array onto the
heap so static row buffers in the page modules can be reused
immediately, and returns. `gPageSwapActive` blocks concurrent taps from
queueing a second worker while one is in flight.

The CREATE envelope is built into a heap-allocated 2 KB buffer
(`sendCreateListAndWait` in [Optional_EvenG2.cpp](components/hardwareone/Optional_EvenG2.cpp)).
Stack-allocating it would eat most of the worker's 4 KB stack.

### Echo guards: SYSTEM_EXIT and DISPLAY_OFF

When **we** issue Shutdown ourselves, the firmware emits both events
shortly after — `SYSTEM_EXIT` (sid=0xE0 Cmd=2 SysEvent EventType=7) and
the 4-byte `DISPLAY_OFF` on sid=0x0D. Both handlers, if left to their
default behavior, would tear our local hijack state down (clearing
`hijackActive`) and/or call `sendHijackShutdown`, which contends with
the worker's CREATE on the write mutex.

Both handlers therefore consult `gOurShutdownAtMs` and ignore the event
if `millis() - gOurShutdownAtMs < 2000`. The variable is forward-declared
near the top of [Optional_EvenG2.cpp](components/hardwareone/Optional_EvenG2.cpp)
because the handlers and the worker live in different sections of the
file. It's set to `millis()` immediately before `sendEnvelope(shutBuf)`
and cleared back to `0` in the worker's `cleanup:` label.

### Per-page test results (2026-04-24, captured pair G2_32)

| Page | Items | Result |
|------|-------|--------|
| Hijack root menu | 6 | ✓ initial CREATE on launch |
| Status | 4 | ✓ via SHUTDOWN+CREATE |
| System | ~6 | ✓ via SHUTDOWN+CREATE |
| Files | varies | ✓ via SHUTDOWN+CREATE (browse navigation works) |
| Network | ~5 | ✓ via SHUTDOWN+CREATE |
| **Sensors** | **12** | ✗ **CREATE-list build failed** |

The Sensors failure mode: `g2BuildCreateListPage` returns 0 because the
encoded protobuf for 12 sensor rows (each carrying a label + numeric
value string) exceeds the 2 KB heap buffer in `sendCreateListAndWait`.
Since Shutdown was already sent before the CREATE encode was attempted,
the firmware is left in a torn-down state with no replacement — lens
goes dark and the hijacked app cannot be re-entered until the user
back-navigates from the firmware's "Connection lost" screen.

Diagnostic log signature:

```
[G2] g2ShowTextAsList: 12 rows shown
[G2] Hijack: Sensors tapped — list shown (228 B)
[G2] page-swap: ShutdownPage sent
EvenCore ShutdownResp (cmd=10 magic=204) res=10 (err)
SYSTEM_EXIT during our own page-swap — ignoring
DISPLAY_OFF during our own page-swap — ignoring
[G2] CREATE-list: build failed (12 items)
[G2] page-swap: CREATE-list failed — hijack state may be inconsistent
```

The "228 B" is the rendered row-buffer size, not the encoded packet
size. The encoded packet (varint tags, length prefixes per item,
container metadata) is several × larger.

### Known gaps / next steps

- **Sensors page**: bump `kBufSize` in `sendCreateListAndWait` to 4096,
  or trim sensor rows to <40 chars each, or paginate to ≤8 items per
  page. The 4096 bump is the smallest fix and matches the per-CREATE
  worst-case we expect from the other pages.
- **Failed-CREATE recovery**: when CREATE fails after Shutdown was
  already sent, the worker should attempt to re-CREATE with the prior
  page's items (or at minimum re-CREATE the hijack root menu) so the
  user lands somewhere usable rather than at "Connection lost".
- **Phase E — formal hijack FSM**: the current implementation accreted
  as patches (echo guards, worker task, deep-copy, settle delay added
  one at a time as bugs surfaced). State now lives across
  `gPageSwapActive`, `gOurShutdownAtMs`, `arm->containerReady`,
  `arm->containerIsList`, `arm->pluginDead`, `g2LensSetContainer(...)`,
  and the hijack module's own `hijackActive` flag. A single explicit
  state machine (`HijackState { Idle, Hijacked, PageSwapShuttingDown,
  PageSwapCreating, PluginDead }`) with a single mutex and explicit
  transitions would make the failure modes above easier to reason about
  and test.

## Unverified third-party claims

The following points came from a single anonymous Discord comment about
G2 image streaming. **Not verified against our hardware or the
g2-kit-unofficial source.** Treat as hypotheses to confirm before relying
on. Captured here so they don't get lost; revisit when we tackle images.

- **~4 KB firmware reassembly buffer ceiling — corroborated, not
  proven.** Claim: the firmware has a fixed-size reassembly buffer
  "a bit under 4 KB"; multi-fragment messages whose total pb body
  exceeds that get dropped, even though every individual fragment
  passed CRC. The g2-kit-unofficial reference's `ble/image.ts`
  hardcodes `maxFragmentSize = 4096` with the inline comment
  "firmware cap; 6144+ rejected with error 7". That's a second source
  saying the same number, but it's still reverse-engineered and
  could trace back to the same upstream reasoning. Treat as a
  practical guideline — don't push above ~3.5 KB pb body without
  testing — but don't claim it's a hardware fact. Our multi-fragment
  sender's 8 KB heap buffer in `sendCreateListAndWait` should grow a
  loud warning above ~3.5 KB so we notice if we ever bump into the
  ceiling for real.
- **Length-tag/data-size mismatch ⇒ zero-padding.** Claim: an envelope
  whose declared `len` exceeds the data actually sent gets zero-padded
  by the firmware up to the declared length, allowing a crude RLE for
  zero runs at fragment boundaries in image data. Plausible (it would
  fall out of a naive memset-then-memcpy reassembler) but unverified.
- **Two ACK layers, both optional.** Claim: writes are acked at the
  BLE-GATT layer (toggleable via WRITE_TYPE_NO_RESPONSE) and again at
  the application/protobuf layer (toggleable by setting MagicRandom=0,
  which makes the firmware skip the response packet entirely). Doesn't
  contradict anything we've observed — we've only ever seen acks for
  nonzero magic values — but our reference's `MagicRandom` doc doesn't
  call out the magic=0 special case. Would be useful for image-fragment
  pipelining if true.
- **Image-tile sync trick.** Claim: when updating all 4 tiles of a
  full-lens image, send all-but-last fragment of each tile first, then
  send the final fragment of each tile, so all four flip on lens at the
  same redraw. Empirical, no protocol docs to cross-check.
- **App-ack-off + flood ⇒ disconnect.** Claim: disabling the app-layer
  ack and sending image fragments as fast as the phone stack accepts
  them eventually overflows a buffer in the headset, dropping the BLE
  connection. Suggests the firmware's reassembly/render queue is
  small enough to be a real backpressure target — relevant for any
  future "stream as fast as possible" image path.
- **~2.5 s for a 4-tile full-lens update.** Claim: even with all known
  pipelining tricks, a full-lens image refresh takes ~2.5 s. Aligns
  with the "8.8 KB/s, ack-latency-bound" observation already documented
  in the Image Geometry section above.

If we end up implementing image streaming, these are the first things
to test. Until then, do not assume any of them.

## Reference

- https://github.com/Commute773/g2-kit-unofficial — MIT, reverse-engineered
  from the shipping com.even.sg Flutter AOT plus live captures. Source of
  truth for everything in this document that isn't tagged "observed".
- `ble/envelope.ts`, `ble/crc.ts`, `ble/session.ts`, `ble/messages.ts`,
  `ble/gen/EvenHub_pb.ts`, `ble/gen/g2_setting_pb.ts` are the files that
  informed most of the above.
