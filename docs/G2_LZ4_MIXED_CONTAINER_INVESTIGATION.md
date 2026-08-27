# G2 LZ4 + multi-container investigation

**Status:** Solo CREATE-image + LZ4 paints. Mixed CREATE (list/text + image child)
+ LZ4 acks (`ImageRawResp`) but **does not paint**. Same mixed path with **raw**
paints (operator-confirmed 2026-07-31 Com2). Production Health/Maps/Pet and
Com2 probes force raw via `G2ImgLz4AutoOffGuard`. Solo LZ4 stays under
Compression (Q32).

**Glasses FW under test:** `2.2.6.10`  
**Host date:** 2026-07-31  
**Related:** `docs/G2_LZ4_COMPRESSION_TEST.md`, `2026-07-19-sdk-image-text-playbook.md`,
`components/hardwareone/System_Lz4.{h,cpp}`, `G2_Glasses.cpp` (prepare + Q32/Com2)

---

## 1. Problem (one sentence)

`CompressMode=2` + LZ4 frame+content-size works for a **solo image CREATE**, but
the same wire encoding on an **image child of a mixed CREATE** returns success
acks and leaves the image region blank.

---

## 2. Ground truth that still holds

| Fact | Evidence |
|---|---|
| Framing that paints (solo) | `G2_LZ4_WRAP_FRAME_CSIZE` — `lz4CompressFrame(src, len, dst, cap, /*contentSize*/ true)` |
| Mode stamp | Field 5 `CompressMode = 2` (`G2_IMG_COMPRESS_LZ4`) |
| `MapTotalSize` | **Compressed** length (not decompressed). Set from wire buffer length in chunkers |
| Magics | Must be ≤255 (`MagicRandom` is uint8). Overflow silent-drops CREATE |
| Pixel payload (our path) | Full **4-bpp BI_RGB BMP file** (`BM…`), not raw nibbles |
| Ack ≠ paint | Playbook + our Health/Maps/Com2 runs: `ImageRawResp` OK, lens blank |
| Mixed + **raw** paints | Pre-LZ4 Health/Maps/Pet and historical Q16/Q17/Q18 (CompressMode=0) |
| Solo + **LZ4** paints | Q32 ART: `20854 → ~317 B`, 1 frag, bars move (~68 ms) |

---

## 3. Architecture of our send path

### 3.1 Central helper (production + most probes)

```cpp
// G2_Glasses.cpp — gG2ImgLz4Auto default true
static bool g2ImgPrepareWirePayload(const uint8_t* bmp, size_t bmpLen,
                                    uint32_t requestedMode, bool autoLz4,
                                    const uint8_t** outPtr, size_t* outLen,
                                    uint32_t* outMode,
                                    uint8_t** outOwnedCompressBuf);
```

Behavior:

1. `requestedMode == G2_IMG_COMPRESS_LZ4` → **passthrough** (Q32 already wrapped).
2. `requestedMode == RAW && autoLz4` → `lz4CompressFrame(..., contentSize=true)`;
   use compressed **only if** `compLen > 0 && compLen < bmpLen`; stamp mode 2.
3. Else → raw BMP, mode 0.

Wired into:

- `sendImageBmpMultiFragment` (CREATE-image + push)
- `sendImageBmpFragmentsNoCreate` (push into already-CREATEd image child)

Log line when compression wins:

```
[ImgPush] wire: LZ4 20854 -> 224 B (cmode=2, enc 1 ms)
```

Kill switch / RAII:

```cpp
static bool gG2ImgLz4Auto = true;
struct G2ImgLz4AutoOffGuard {
  bool prev;
  G2ImgLz4AutoOffGuard() : prev(gG2ImgLz4Auto) { gG2ImgLz4Auto = false; }
  ~G2ImgLz4AutoOffGuard() { gG2ImgLz4Auto = prev; }
};
```

### 3.2 What ImageRawResp handling does today

`G2ResKind::AckOnly` for `cmd==4` only matches **magic range** and increments
`gImgPushAcked`. It does **not** decode the inner `ErrorCode` / result enum from
the ImgRaw response body. So “push complete OK” means “got matching magic
notify,” not “decompressed and painted.”

---

## 4. Approaches tried (chronological)

### A. Framing sweep (Q32f) — **SUCCESS for solo**

**Where:** Test Suite → Image → Compression (LZ4) → `Q32f` *(menu retired 2026-07-31)*  
**Code:** was `g2ProbeImageQ32fLz4FormatSweep`, `g2Lz4WrapPayload`, enum `G2Lz4Wrap`

Tried four byte layouts under `CompressMode=2`, same 288×144 ART BMP:

| Wrap | Encoder call | Solo paint? |
|---|---|---|
| frame + content size | `lz4CompressFrame(..., true)` | **Yes** (chosen) |
| frame, no csize | `lz4CompressFrame(..., false)` | Yes (seen before dismiss) |
| block + u32 LE size prefix | 4-byte len + `lz4CompressBlock` | Not confirmed |
| bare block | `lz4CompressBlock` | Not confirmed |

Control leg: raw mode 0 must paint or the run is void.

**Difference from mixed:** Q32f uses **solo CREATE-image** via
`sendImageBmpMultiFragment` + `G2ImgLz4AutoOffGuard` so legs stay intentional.

---

### B. A/B benchmark (Q32) — **SUCCESS for solo**

**Where:** Compression (LZ4) → `Q32: A/B (frame+csize)`  
**Code:** `g2ProbeImageQ32Lz4Benchmark`, `buildLz4ArtBmp` / `buildLz4NoiseBmp`

Same container, CREATE once, then timed pushes:

- ART raw vs ART LZ4 (frame+csize)
- NOISE raw vs NOISE LZ4

Result (bench): ART LZ4 ~66× smaller / ~36× faster and **visually paints**.
Noise does not shrink (stays ~6 frags) — expected.

---

### C. Global auto-LZ4 in both chunkers — **PARTIAL**

**Code:** `g2ImgPrepareWirePayload` integrated into both senders; default on.

Intent: every RAW caller inherits FRAME_CSIZE when smaller; photos/noise stay raw.

**Observed:**

| Path | Wire | Ack | Paint |
|---|---|---|---|
| Q32 solo ART | LZ4 | OK | **Yes** |
| Health graph (list+text+image child) | `20854 → ~753 B`, cmode=2 | ImageRawResp OK | **No** |
| Maps image child | `20854 → ~11 KB`, 3–4 frags | OK | **No** |
| Pet image child | (same pattern) | OK | **No** (assumed; guarded with Health/Maps) |

Magics on Health/Maps were already correct (create **242** / push **243**, uint8 band).
Not a magic overflow regression.

---

### D. Force raw on production mixed pages — **WORKAROUND (current)**

**Code sites** (`G2ImgLz4AutoOffGuard` around `sendImageBmpFragmentsNoCreate`):

- Health graph push in `g2HealthPageWorker` (~`G2_Glasses.cpp:19806`)
- Map page image push (~`19253`)
- Pet page image push (~`19429`)

Comment at Health:

```cpp
// Force RAW here. Auto LZ4 (FRAME_CSIZE) acks on this mixed
// list+text+image child (create=242 / push=243) but does not
// paint — same "ack ≠ paint" trap as the LZ4 probes. Solo
// CREATE-image (Q32) paints under LZ4; the 3-pane health
// path does not.
G2ImgLz4AutoOffGuard healthGraphRaw;
```

Restores paint for those pages. Does **not** explain why mixed fails.

---

### E. Com2 under auto LZ4 — **FAIL paint (2026-07-31)**

**Where:** Test Suite → Image → Com2 (then still labeled LZ4 multi)  
First run: probes used auto LZ4 (no AutoOffGuard yet).

| Probe | CREATE | Wire | Frags | Ack | Paint |
|---|---|---|---|---|---|
| Q16 | 226 | 20854→**224**, cmode=2 | 1 | OK | **No** |
| Q17 | 226 | 20854→224, cmode=2 | 1 | OK | **No** |
| Q18 | 226 | 3318→**155**, cmode=2 | 1 | OK | **No** |
| Q28 / Q28L / Q30* | 226 | LZ4, cmode=2 | 1 | OK | **No** |

Resp echoed `CompressMode=2` + compressed `MapTotalSize` — envelope accepted,
eyes blank.

### E2. Com2 forced RAW — **PAINT OK (2026-07-31, same FW)**

Com2 probes wrapped with `G2ImgLz4AutoOffGuard`. Menu was **Com2 (multi raw)**
(now **Mixed / multi**; force-raw only on Q16–Q18 controls).

Log signature for Q16:

```
Q16 BMP 20854 B (288x144 4bpp)
Q16 frag 1/6 … cmode=0          ← no [ImgPush] wire: LZ4
… 6/6 sent, 6/6 acked in ~2443 ms
```

Operator: **images paint exactly as they should** (Q16, Q17, Q18 confirmed;
Q28/Q28L/Q30 also raw on this flash).

| | LZ4 mixed (E) | RAW mixed (E2) |
|---|---|---|
| CREATE | same 226, same geom | same |
| Payload | FRAME_CSIZE of 4bpp BMP | raw 4bpp BMP |
| cmode | 2 | 0 |
| Frags (288×144) | 1 (~224 B) | 6 (20854 B) |
| ImageRawResp | OK | OK |
| Lens image | blank | **paints** |

**Locked conclusion (updated after Q16d):** multi-container CREATE is fine.
Mixed image children reject **FRAME_CSIZE** under mode 2 but accept **bare
LZ4 block** of BMP (Even App `dart_lz4` shape). Solo CREATE-image still
paints under FRAME_CSIZE (Q32); production auto now uses bare block for
both paths.

### What’s still missing (next experiments)

1. ~~**H1 bpp:**~~ **FAIL paint** (Q16a)
2. ~~**H1 packed4:**~~ **FAIL paint** (Q16b)
3. ~~**H4 image-first:**~~ **FAIL paint** (Q16c, still FRAME_CSIZE)
4. ~~**H3 bare block:**~~ **PAINTS** (Q16d — list + image visible)
5. **Production:** auto LZ4 switched to bare block; Health/Maps/Pet force-raw removed
6. **H7 optional:** BLE sniff to confirm phone MapRawData matches block

**Menu prune (2026-07-31):** Image test lists dropped retired chase probes
(Q16a–c, Q30b, Q32f + non-block Q32 rows, Q29, Q31 REBUILD). Remaining
canaries: Compression → Q32 bare-block A/B; Mixed → Q16/Q16d/Q17/Q18/Q28*/Q30/Q30c.
Findings above stay as the historical record.

### Phone app mining (2026-07-31, Pixel Fold / com.even.sg 2.2.7)

Live `libapp.so` SHA256 matches `/Users/morgan/even-app-extract` (Play pull).
Installer `com.android.vending`. Key strings / packages:

| Finding | Implication |
|---|---|
| `package:dart_lz4` + `lz4BlockCompress` | Even App uses **bare LZ4 block**, not frame |
| `_evenHubBmp4FromGray4Pixels` / `compressBmpData` / `compressMode: 2` | Phone path: gray4 → **BMP4** → LZ4 → cmode=2 |
| `PureDartLz4FastEngine`, `BmpCompression` | Matches `docs/FlutterApp-main/.../evenhub_image_codec.dart` |
| Hub SDK README mixed example sets `zOrderIndex` on every child | Optional all-or-nothing; our CREATE omits it (valid per SDK) |
| No `0x184D2204` magic in `libapp.so` | Consistent with block (not frame) on the phone |

Codec reference (bottom-up BMP4, palette `i*17`):
`docs/FlutterApp-main/lib/src/services/evenhub/evenhub_image_codec.dart`

**Resolved:** Health/Maps/Pet force-raw removed; auto LZ4 is bare-block.
Flash and confirm Health graph / Maps / Pet paint under compression
(`[ImgPush] wire: LZ4-block …`).

---

### F. External research (not on-wire experiments)

#### even_hub_sdk 0.0.12

- TS stamps `compressMode: 2`; **actual LZ4 bytes live in closed native bridge**.
- Official README / playbook examples include **mixed list+text+image** then
  `updateImageRawData` — stock firmware **should** support mixed+LZ4 if the
  phone path does.
- Playbook data-format note (important fork):

| Path | What paints on phone host |
|---|---|
| High-level `updateImageRawData` (mode 2) | raw **byte-per-pixel** `number[]` (0–255), not BMP/PNG |
| Low-level bridge without compressMode | PNG **file bytes** |

Our firmware path always sends **4-bpp BMP file bytes**, mode 0 or 2. Solo mode 2
+ BMP paints on glasses — so BMP+LZ4 is valid for **solo**. Unknown whether the
phone’s mixed path LZ4s **BMP** or **byte-per-pixel / gray4** and whether that
difference only shows up under mixed compositing.

#### faceclaw (jimrandomh) — **not a stock LZ4 teacher**

Custom CFW: RLE+zlib, `CompressMode=0`. Notes that `CompressMode!=0` hits a
1bpp expander on their fork. Do not treat as ground truth for stock 2.2.6.10.

#### Playbook ack≠paint

Full sweeps with every write `success` and nothing painted — power-cycle /
firmware wedge can look identical. Less likely here because **raw mixed still
works** on the same session after AutoOffGuard, and Com2 list/text keep updating.

---

## 5. Hypotheses still open (not tried, or only partially)

Ordered by how much they would change the wire vs. how cheap they are to A/B.

### H1 — Payload shape under mode 2 differs from mode 0 (phone path)

**Idea:** Solo BMP+FRAME_CSIZE paints; mixed decompress path may expect
byte-per-pixel / packed gray4 / 8bpp buffer that then goes through
`imageToGray4`, while mode 0 still format-detects BMP.

**Try:**

1. Build uncompressed **byte-per-pixel** buffer (W×H bytes, 0–255), LZ4
   FRAME_CSIZE that, stamp mode 2, push into Q16 image child.
2. Same with **packed 4bpp without BMP header** (raw nibbles only).
3. Keep Q16 CREATE geometry identical; only MapRawData content changes.
4. Control: same buffer as raw mode 0 (expect fail or tile garbage per playbook).

**Code (2026-07-31):** Com2 → `Q16a: mixed + LZ4 byte/px`
(`g2ProbeImageQ16aMixedLz4BytePerPixel`). CREATE matches Q16; push is
pre-encoded FRAME_CSIZE of 288×144 bpp stripes, `cmode=2`.

**On-device result (Q16a): FAIL paint.** Log:
`bpp 41472 B → LZ4 FRAME_CSIZE 220 B`, `cmode=2`, 1/1 acked in 114 ms,
list dismissable — image area blank. Same ack≠paint as LZ4-of-BMP.
Phone-path bpp is **not** the missing piece for mixed children.

**On-device (Q16b): FAIL paint** (user). Packed-4bpp FRAME is not the fix.

**On-device (Q16c): FAIL paint** (user). Image-first CREATE + FRAME_CSIZE
of BMP still blank.

### H2 — Inner ImageRawResp ErrorCode is nonzero / soft-fail

**Idea:** We treat any cmd=4 magic match as success; body may carry
`ImgRawFail` / `imageToGray4Failed` / similar.

**Try:** Decode ImgRaw response field(s) in the cmd=4 handler (or dump full
`pb` hex for Com2 pushes). Log `ErrorCode` / enum name beside ack count.

**Code (2026-07-31):** AckOnly cmd=4 path now logs
`ImageRawResp ErrorCode f8=…` (or `default=0` if f8 absent). Q16a ack hex
showed f1=cid, f2=name, f4=220, f5=cmode=2 — f8 not obvious in truncated
head; re-run any push and read the new line.

### H3 — Framing that works solo is wrong for mixed

**Idea:** Mixed decoder wants bare block (Even App) or size-prefix / no-csize.

**Phone evidence:** app uses `dart_lz4` **block**, while our auto path and
Q16a–c use **FRAME_CSIZE**. Solo FRAME paints; mixed FRAME blanks — framing
fork is newly plausible.

**Code:** Com2 → `Q16d: mixed + LZ4 bare block`
(`g2ProbeImageQ16dMixedLz4Block`). Log: `LZ4 bare-block … cmode=2`.

**On-device (2026-07-31): PASS — image and list both painted.** This is the
fix. Auto path + Health/Maps/Pet now use `lz4CompressBlock`.

### H4 — `zOrderIndex` / declaration order / child index

**Idea:** SDK 0.0.12 added `zOrderIndex` (all-or-nothing, unique). Mixed LZ4
paint might depend on z-order or image-first wire order.

**Tried partially:** Q30 vs Q30b (list→text→image vs list→image→text) — both
acked blank under LZ4; list still first. Not a full zOrderIndex implementation.

**On-device (Q16c): FAIL paint.** Declaration order was not the fix;
framing was (see H3 / Q16d).

### H5 — First push after mixed CREATE needs raw warmup then LZ4

**Idea:** g2-kit “sacrificial warmup”; Health comment rejected double-push for
radio reasons, but a **raw first frame + LZ4 thereafter** might prime the
decompressor.

**Try:** Q28/Q28L: push 1× raw (AutoOff), then LZ4 stream; eyes on whether
frame 2+ paint.

### H6 — Session / MapSessionId / magic band interaction with compression

**Idea:** Unlikely (acks echo sid/total/cmode correctly) but worth one A/B:
fixed session id 1 vs bumping; create/push magics in Q6 band vs 226/227.

### H7 — BLE sniff Even App mixed+LZ4 reference

**Idea:** Capture phone → glasses Cmd=3 for a stock Even App / Hub SDK mixed
page with image update. Diff: BMP vs bpp, frame header bytes, CompressMode,
MapTotalSize, CREATE child flags.

**Try:** nRF sniffer / similar; compare first 32 bytes of MapRawData to our
`04 22 4D 18` LZ4 frame magic + descriptor.

### H8 — Confirm Com2 under forced raw still paints (control)

**Idea:** Prove the Com2 build didn’t break CREATE geometry.

**Try:** Temporary `G2ImgLz4AutoOffGuard` inside Q16 only — expect paint.
If raw Q16 is blank too, bug is elsewhere (not LZ4).

---

## 6. What we believe is *not* the bug

| Ruled out / weak | Why |
|---|---|
| Magic >255 | Com2 uses 226/227; Health 242/243; CreateSuccess |
| MapTotalSize = decompressed size | Echo shows 224/160/155 matching compressed |
| “Multi-container can never take images” | Raw mixed paints; list events fire |
| Wrong CompressMode stamp | Resp echoes `28 02` (cmode=2) |
| Our LZ4 encoder broken in general | Solo Q32 paints; host round-trip vs liblz4 in Unit tests (System_Lz4) |
| Left temple plugin-silent | Right temple carries EvenCore; probes complete on R |

---

## 7. Suggested next runbook (for whoever picks this up)

Minimum A/B set on one glasses unit (FW noted in log):

1. **Com2 → Q16 with AutoOff (raw)** — expect paint (control).
2. **Com2 → Q16 LZ4 FRAME_CSIZE BMP** — expect blank (repro; already done).
3. **Decode ImageRawResp ErrorCode** on that push — log full pb.
4. **Q16 with LZ4 of byte-per-pixel buffer** (H1) — paint?
5. **Q16 with LZ4 frame no-csize of same BMP** (H3) — paint?
6. **Q28: 1× raw then LZ4** (H5) — paint on later frames?
7. If still blank: BLE sniff Even App (H7).

Do not re-enable Health/Maps/Pet auto LZ4 until a mixed probe paints under eyes.

---

## 8. File / symbol index

| Piece | Location |
|---|---|
| Auto prepare | `G2_Glasses.cpp` `g2ImgPrepareWirePayload`, `gG2ImgLz4Auto` |
| Chunkers | `sendImageBmpMultiFragment`, `sendImageBmpFragmentsNoCreate` |
| Health/Map/Pet raw guard | `G2ImgLz4AutoOffGuard` at those workers |
| Q32/Q32f | `g2ProbeImageQ32*`, `g2Lz4WrapPayload`, `G2_LZ4_WRAP_*` |
| Com2 menu | `G2_Page_TestSuite.cpp` `TEST_LEVEL_IMAGE_COM2`, `buildImageCom2Rows` |
| Mixed probes | `g2ProbeImageQ16*` … `Q30*` in `G2_Glasses.cpp` / decls in `G2_Glasses.h` |
| Encoder | `System_Lz4.cpp` `lz4CompressFrame` |
| Schema notes | `System_G2_Protocol.h` `g2BuildImageRawBody`, `G2_IMG_COMPRESS_*` |
| Solo bench doc | `docs/G2_LZ4_COMPRESSION_TEST.md` |
| SDK field notes | `2026-07-19-sdk-image-text-playbook.md` |

---

## 9. Operator summary (latest Com2 session)

All Com2 probes exercised under **auto LZ4 FRAME_CSIZE on 4bpp BMP**:
CREATE OK, wire shrink OK, ImageRawResp OK, **no image paint**. List/text
interaction remained live. Production pages stay on **forced raw** until a
mixed paint path is found.
