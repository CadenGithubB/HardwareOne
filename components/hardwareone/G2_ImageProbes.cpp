// =============================================================================
// G2 glasses: image probes (the Q series of protocol experiments)
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "System_Deadline.h"  // deadlineAfter / deadlinePending: the pacing waits survive the millis() wrap
#include "System_Lz4.h"
#include "BLE_Server.h"
#include "System_MemUtil.h"
#include "System_VFS.h"
#include "G2_HijackCmd.h"

#include "System_Settings.h"
#include "G2_GlassesInternal.h"

// Pretty banner — gives a visual divider in the log so the operator can
// scroll up to a probe's "begin" marker and read forward to its "done".
// The "what to grep for" lines name the magic range each probe uses; the
// firmware echoes our magic in every response, so grep on `magic=NNN` to
// pull out the matching ack from the rest of the BLE chatter.
static void probeBanner(const char* probe, uint32_t createMagic,
                        uint32_t followMagicLo, uint32_t followMagicHi,
                        const char* expect) {
  // All DEBUG_G2F: one gate check instead of nine.
  if (!(getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_G2))) return;
  DEBUG_G2F("[ImgProbe] ───────── %s ─────────", probe);
  if (createMagic) {
    DEBUG_G2F("[ImgProbe] match CreateResp cmd=1 magic=%u",
              (unsigned)createMagic);
  } else {
    DEBUG_G2F("[ImgProbe] CreateResp uses the per-transaction rotating magic logged at TX");
  }
  if (followMagicLo) {
    DEBUG_G2F("[ImgProbe] follow-on response magic=%u..%u (Cmd=4 image ACK or Cmd=8 REBUILD)",
              (unsigned)followMagicLo, (unsigned)followMagicHi);
  }
  DEBUG_G2F("[ImgProbe] expect: %s", expect);
  DEBUG_G2F("[ImgProbe] decode hint: in 'RX notify ... head=[AA 12 .. 08 CC 10 MM ...]'");
  DEBUG_G2F("[ImgProbe]   CC=01 CreateResp, CC=04 ImageRawResp, CC=08 RebuildResp;");
  DEBUG_G2F("[ImgProbe]   MM=our magic (low byte). Bytes after MM = response body —");
  DEBUG_G2F("[ImgProbe]   look for varint after a tag byte; that IS the error code:");
  DEBUG_G2F("[ImgProbe]   0=CreateOK 1=InvalidContainer 2=Oversize 3=OOM 5=ImgRawFail");
  DEBUG_G2F("[ImgProbe]   6=RebuildOK 7=RebuildFailed.");
}

static void probeFooter(const char* probe, unsigned okCount, unsigned total) {
  // All DEBUG_G2F: one gate check instead of two.
  if (!(getLogLevel() >= LOG_LEVEL_DEBUG && isDebugFlagSet(DEBUG_G2))) return;
  DEBUG_G2F("[ImgProbe] %s done — %u/%u candidates shipped", probe, okCount, total);
  DEBUG_G2F("[ImgProbe] ─────────────────────────────────────");
}

// Q4 — CREATE-image with full-tile dimensions and a fresh CID. NO
// follow-on Cmd=3 burst. Why: empirical test on 2026-04-26 showed that
// a CREATE-image with 32×12 dims + CID=1 was rejected with
// `CreateResp res=1 CreateInvalidContainer`, which the firmware
// followed with `ABNORMAL_EXIT(6)` + `SYSTEM_EXIT(7) reason=0`,
// killing the hijacked Blocks widget's plugin task. The follow-on
// Cmd=3 frames went into a dead pipe, the picker rebuild timed out,
// BLE disconnected — operator hard-restart required. Lesson: a failed
// CREATE-image kills the entire hijack context. So Q4 now does ONLY
// the CREATE step with safer parameters (full-tile dimensions per the
// doc's stated minimum, CID=2 to avoid conflict with the picker list
// at CID=1) so we can isolate which parameter the firmware objects to
// without cascading into a disconnect.
//
// If THIS Q4 also returns CreateInvalidContainer, the issue is the
// Blocks widget's slot-type lock (image containers may only be
// accepted by a different widget context entirely) — at which point
// we need a different reverse-engineering angle (HCI snoop the phone
// app's image push to see what widgetId/Cmd=17 sequence it uses).
const char* g2ProbeImageQ4Lifecycle() {
  EXT_RAM_BSS_ATTR static char ret[200];

  G2Temple* arm = pickEvenAiArm("imgQ4");
  if (!arm) return "Img Q4: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic = allocCreateMagic();
  const uint32_t kImageW      = 288;
  const uint32_t kImageH      = 144;   // full single tile (per doc)
  const uint32_t kCid         = 2;     // distinct from picker list/text

  probeBanner("Q4: CREATE-image (full tile, CID=2)",
              kCreateMagic, 0, 0,
              "expect Cmd=1 CreateResp matching the logged CREATE magic, "
              "ErrorCode=0 (CreateSuccess). "
              "Differs from Q4b only in CID (Q4=2, Q4b=1). If Q4 succeeds "
              "and Q4b doesn't, the picker's CID=1 list residue is "
              "blocking image re-use. If both fail with res=1, the "
              "Blocks widget slot rejects image containers entirely — "
              "next step is HCI-snooping the phone app's image flow "
              "to find an image-capable widget context.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q4: pre-burst SHUTDOWN failed";

  uint8_t buf[128];
  size_t envLen = g2BuildCreateImage(
      allocSeq(), kCreateMagic,
      /*containerName*/ "imgQ4", /*containerId*/ kCid,
      /*x*/ 0, /*y*/ 0, /*w*/ kImageW, /*h*/ kImageH,
      /*widgetId*/ BLOCKS_WIDGET_ID,
      buf, sizeof(buf));
  if (envLen == 0) return "Img Q4: CREATE-image build failed";

  noteOurShutdownSent();
  DEBUG_G2F("[ImgProbe] Q4 CREATE-image magic=%u (%u×%u @ 0,0, CID=%u, "
            "name='imgQ4', widgetId=%u)",
            (unsigned)kCreateMagic,
            (unsigned)kImageW, (unsigned)kImageH,
            (unsigned)kCid, (unsigned)BLOCKS_WIDGET_ID);
  logPbHex("Q4 env", buf, envLen);

  bool ok = sendEnvelope(*arm, buf, envLen);
  if (!ok) DEBUG_G2F("[ImgProbe] Q4 — TX failed");

  // Hold visible 2 s so an accepted CREATE has time to render its empty
  // rectangle on lens before we tear it down.
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Clean up the image container before the worker rebuilds the picker.
  // Otherwise CREATE-list on the same widgetId fails (image still live).
  probePostProbeShutdown(cleanupFence);

  probeFooter("Q4", ok ? 1 : 0, 1);
  snprintf(ret, sizeof(ret),
           "Img Q4: %s — watch RX cmd=1 magic=%u ErrorCode (0=success)",
           ok ? "1/1 sent" : "TX failed", (unsigned)kCreateMagic);
  return ret;
}

const char* g2ProbeImageDocSummary() {
  // No TX — dumps the verified image-protocol schema and the now-known
  // facts so the operator can read them next to a probe's response in
  // the same log stream. Schema fields confirmed against
  // g2-kit-unofficial/ble/gen/EvenHub_pb.ts (verified 2026-04-26).
  DEBUG_G2F("[ImgProbe] === Image protocol — verified schema ===");
  DEBUG_G2F("[ImgProbe]   Cmd=0 CREATE_STARTUP carries CreateStartUpPageContainer{");
  DEBUG_G2F("[ImgProbe]     f1=ContainerTotalNum, f2=ListObject, f3=TextObject,");
  DEBUG_G2F("[ImgProbe]     f4=ImageObject, f5=widgetId }");
  DEBUG_G2F("[ImgProbe]   ImageContainerProperty (inside f4) {");
  DEBUG_G2F("[ImgProbe]     f1=XPosition, f2=YPosition, f3=Width, f4=Height,");
  DEBUG_G2F("[ImgProbe]     f5=ContainerID, f6=ContainerName }");
  DEBUG_G2F("[ImgProbe]     — NO Format/bpp field; 4-bpp indexed is implicit.");
  DEBUG_G2F("[ImgProbe]   Cmd=3 ImgRawMsg (wrapper f5) ImageRawDataUpdate {");
  DEBUG_G2F("[ImgProbe]     f1=ContainerID, f2=ContainerName, f3=MapSessionId,");
  DEBUG_G2F("[ImgProbe]     f4=MapTotalSize, f5=CompressMode (0=raw),");
  DEBUG_G2F("[ImgProbe]     f6=MapFragmentIndex, f7=MapFragmentPacketSize,");
  DEBUG_G2F("[ImgProbe]     f8=MapRawData (bytes) }");
  DEBUG_G2F("[ImgProbe]   Cmd=4 ImgResCmd (wrapper f6 — NOT f5)");
  DEBUG_G2F("[ImgProbe]     ResponseImageRawDataCmd { f1..f7 echo of request,");
  DEBUG_G2F("[ImgProbe]     f8=ErrorCode (varint, EvenHub_ErrorCode_List) }.");
  DEBUG_G2F("[ImgProbe]     NOTE: image responses don't use ResCmdMsg — read");
  DEBUG_G2F("[ImgProbe]     ErrorCode at inner f8 directly.");
  DEBUG_G2F("[ImgProbe]   Pixel format: 4-bpp indexed, 16-entry palette,");
  DEBUG_G2F("[ImgProbe]     2 px/byte, HIGH NIBBLE FIRST, row-major top-down.");
  DEBUG_G2F("[ImgProbe]   Full lens 576x288 = 2x2 grid of 288x144 tiles.");
  DEBUG_G2F("[ImgProbe]   Per-Cmd=3 pb body <=4 KB (firmware reassembly cap).");
  DEBUG_G2F("[ImgProbe]   Image-layer fragmentation via MapSessionId/Index/Total");
  DEBUG_G2F("[ImgProbe]     is SEPARATE from envelope-layer fragmentation.");
  DEBUG_G2F("[ImgProbe]   *** MapRawData MUST be a real BMP file (starts with 'BM').");
  DEBUG_G2F("[ImgProbe]     The firmware does format detection — only BMP accepted.");
  DEBUG_G2F("[ImgProbe]     4-bpp indexed grayscale, BGRA palette, top- or bottom-up.");
  DEBUG_G2F("[ImgProbe]     [Source: jimrandomh @ G2/R1 dev Discord, 2026-04-25.]");
  DEBUG_G2F("[ImgProbe]   CompressMode=0 only — ~100 values probed, all no-op.");
  DEBUG_G2F("[ImgProbe]   Length-mismatch trick: declared len > actual data => firmware");
  DEBUG_G2F("[ImgProbe]     zero-pads up to declared. RLE-of-zeros for tile boundaries.");
  DEBUG_G2F("[ImgProbe]   App-layer ack disabled by setting magic=0 in the wrapper.");
  DEBUG_G2F("[ImgProbe]     WARNING: combining magic=0 with rapid sends overflows the");
  DEBUG_G2F("[ImgProbe]     headset buffer and DROPS the BLE connection.");
  DEBUG_G2F("[ImgProbe]   g2-kit docs: first image after CREATE may need a");
  DEBUG_G2F("[ImgProbe]     sacrificial warmup — on FW 2.2.4.34 the first stream");
  DEBUG_G2F("[ImgProbe]     already paints; Health/UI skips the double-push.");
  DEBUG_G2F("[ImgProbe]   Observed ceiling ~2.5 s for full 4-tile update — firmware");
  DEBUG_G2F("[ImgProbe]     bottleneck is the post-fragment ack delay (lens sync).");
  DEBUG_G2F("[ImgProbe]   All-black images ack faster than non-zero patterns.");
  DEBUG_G2F("[ImgProbe]   4-tile sync trick: send N-1 frags of each of 4 tiles,");
  DEBUG_G2F("[ImgProbe]     then send the final frag of each — synchronises the flip.");
  DEBUG_G2F("[ImgProbe]   Error codes (EvenHub_ErrorCode_List):");
  DEBUG_G2F("[ImgProbe]     0=CreateSuccess 1=InvalidContainer 2=Oversize 3=OOM");
  DEBUG_G2F("[ImgProbe]     4=ImgRawSuccess 5=ImgRawFailed 6=RebuildSuccess");
  DEBUG_G2F("[ImgProbe]     7=RebuildFailed 8=TextSuccess 9=TextFailed");
  DEBUG_G2F("[ImgProbe]     10=ShutdownSuccess 11=ShutdownFailed.");
  return "Img Doc: verified schema dumped to log";
}

// Q5/Q7/Q8 REMOVED 2026-04-27. All three pushed sub-tile BMPs (32×32)
// into a 288×144 container, which the firmware acks but doesn't render
// (verified 2026-04-26: only full-tile BMPs trigger redraw). Q6 covers
// the working path; sub-tile rendering is documented as not supported
// in docs/G2_PROTOCOL.md "Render only fires when BMP dimensions match
// container dimensions."

// ─────────────────────────────────────────────────────────────────────
// Q6 — full-tile (288×144) BMP via multi-fragment Cmd=3 session.
// Container is the same Q4 known-good shape; BMP fills it at native
// container resolution.
// ─────────────────────────────────────────────────────────────────────
// 288×144 4-bpp = 14+40+64+(144*144) = 20854 B, comfortably exceeding
// the ~4 KB per-Cmd=3 reassembly cap. Has to be split into MapRawData
// chunks (G2_IMG_MAPRAW_CHUNK_BYTES) and shipped as a sequential image-layer session: same
// MapSessionId, MapFragmentIndex 0..N-1, MapTotalSize=20854 on every
// fragment, sum of MapFragmentPacketSize equals MapTotalSize on the
// last fragment.
//
// Tests:
//   - image-layer fragmentation works (same session, sequential idx)
//   - real-frame throughput (expect ~2.5 s per discord)
//   - all chunks ack with ErrorCode=4, with a cumulative ImgRawSuccess
//     that triggers the actual lens render
//
// Each Cmd=3 has ~50 B of pb overhead on top of MapRawData; G2_IMG_MAPRAW_CHUNK_BYTES
// keeps the full Cmd=3 pb body under the ~4 KB firmware reassembly cap.
const char* g2ProbeImageQ6BmpMultiFragment() {
  EXT_RAM_BSS_ATTR static char ret[220];

  G2Temple* arm = pickEvenAiArm("imgQ6");
  if (!arm) return "Img Q6: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  // CREATE takes a fresh shared low-byte correlation token. The independent
  // Cmd=3 band 0x27..0x2D covers up to 7 fragments, all ≤255.
  const uint32_t kCreateMagic    = allocCreateMagic();
  const uint32_t kPushMagicBase  = G2_MAGIC_IMAGE_BASE + 0x27;  // 249..255
  // Container parameters — match Q4's known-good shape exactly. CID=2
  // and name="imgQ4" are the values empirically observed to ack
  // (2026-04-26); other CID/name combos silently dropped CREATE.
  const uint32_t kCid            = 2;
  const char*    kCName          = "imgQ4";
  const int32_t  kImgW           = 288;
  const int32_t  kImgH           = 144;
  const size_t   kChunkBytes     = G2_IMG_MAPRAW_CHUNK_BYTES;

  probeBanner("Q6: multi-fragment full-tile BMP (288×144)",
              kCreateMagic, kPushMagicBase, kPushMagicBase + 7,
              "expect Cmd=1 res=0 then a stream of Cmd=4 acks (one per "
              "fragment) all ErrorCode=4. Total ~20.8 KB BMP shipped as "
              "~6 chunks. Throughput ceiling per Discord is ~2.5 s for "
              "a full-tile render.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q6: pre-burst SHUTDOWN failed";

  // Step 1: build the full-tile BMP into the heap. ~21 KB so we can't
  // stack-allocate it.
  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    return "Img Q6: BMP heap alloc failed";
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(cleanupFence);
    return "Img Q6: BMP build failed";
  }
  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  DEBUG_G2F("[ImgProbe] Q6 BMP built: %u B (%dx%d 4bpp), splitting into %u chunks of <=%u B",
            (unsigned)bmpLen, kImgW, kImgH, kFrags, (unsigned)kChunkBytes);
  logPbHex("Q6 BMP head", bmp, bmpLen);

  unsigned okFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q6", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, nullptr);
  free(bmp);

  // Hold visible — the firmware's 4-tile sync delay is on the order of
  // seconds, so render may keep evolving for ~2 s after the last frag.
  vTaskDelay(pdMS_TO_TICKS(kImgPostFragHoldMs));

  probePostProbeShutdown(cleanupFence);

  probeFooter("Q6", okFrags, kFrags);
  snprintf(ret, sizeof(ret),
           "Img Q6: CREATE + %u/%u frags (magic %u then %u..%u) — "
           "watch for cmd=4 stream with ErrorCode=4 across every fragment",
           okFrags, kFrags, (unsigned)kCreateMagic,
           (unsigned)kPushMagicBase, (unsigned)(kPushMagicBase + kFrags - 1));
  return ret;
}

// Q6b — clone of Q6 with double-tap-to-dismiss instead of fixed 3 s
// hold. Same BMP and transport with its own Cmd=3 push band; the only
// change is the hold mechanism. Validated 2026-04-27 with firmware
// 2.2.0.24: hold ends on SysEvent DOUBLE_CLICK(3) (single CLICK does
// not fire while a pure image is on-lens) or 60 s safety cap.
const char* g2ProbeImageQ6bBmpTapDismiss() {
  EXT_RAM_BSS_ATTR static char ret[220];

  G2Temple* arm = pickEvenAiArm("imgQ6b");
  if (!arm) return "Img Q6b: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  // Keep this Cmd=3 band distinct from Q6 for readable push-ACK logs; CREATE
  // correlation itself comes from the shared rotating allocator.
  const uint32_t kCreateMagic   = allocCreateMagic();
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x15;  // 229..235
  const int32_t  kImgW          = 288;
  const int32_t  kImgH          = 144;

  probeBanner("Q6b: full-tile BMP with double-tap-to-dismiss",
              kCreateMagic, kPushMagicBase, kPushMagicBase + 7,
              "same as Q6, but image stays visible until you "
              "DOUBLE-TAP the ring or temple, or the 60 s safety cap "
              "fires. Single tap won't fire SysEvent during image-up "
              "(firmware 2.2.0.24). Watch [G2] IMG probe hold: log "
              "line for dismiss detection.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q6b: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    return "Img Q6b: BMP heap alloc failed";
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(cleanupFence);
    return "Img Q6b: BMP build failed";
  }

  unsigned okFrags = 0;
  unsigned totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q6b", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);

  // Tap-to-dismiss with 60 s safety cap. If the firmware doesn't fire
  // SysEvents during a pure image-up state, the cap fires and we tear
  // down anyway.
  DEBUG_G2F("[ImgProbe] Q6b — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q6b — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(cleanupFence);
  probeFooter("Q6b", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q6b: %u/%u frags, hold ended via %s",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q9 — frame builder API. Builds a full-tile 288×144 BMP from
// composed regions (banner band, body band, footer band) instead of a
// uniform pattern. Demonstrates the shape a real "draw a card on the
// lens" API would take: a writable 4bpp pixel buffer with helpers
// that fill rectangles by palette index. Once Q9 renders cleanly,
// the same bufferDrawRect path can be lifted into a public
// pushTile(arm, bmp288x144) API for feature code.
//
// Layout (palette indices):
//   rows   0..23   index 15 (white)  — header band
//   rows  24..119  stripes pattern   — body band
//   rows 120..143  index 0  (black)  — footer band
// ─────────────────────────────────────────────────────────────────────
static void bmpDrawRect4bpp(uint8_t* pixelRows, int32_t imgW, int32_t imgH,
                            int32_t x, int32_t y, int32_t w, int32_t h,
                            uint8_t paletteIdx) {
  const int32_t absH = imgH < 0 ? -imgH : imgH;
  if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
  if (x + w > imgW || y + h > absH) return;
  // Row stride: 4-bpp packed, 2 px/byte, padded to 4 bytes per row.
  const size_t rawBytesPerRow = (size_t)((imgW + 1) / 2);
  const size_t stride = (rawBytesPerRow + 3) & ~3u;
  const uint8_t pix2 = (uint8_t)((paletteIdx & 0x0F) << 4 | (paletteIdx & 0x0F));
  for (int32_t row = y; row < y + h; row++) {
    uint8_t* rowPtr = pixelRows + (size_t)row * stride;
    int32_t xPx = x;
    while (xPx < x + w) {
      const bool hi = (xPx & 1) == 0;
      const size_t bytePos = (size_t)(xPx / 2);
      if (hi && (xPx + 1) < x + w) {
        rowPtr[bytePos] = pix2;
        xPx += 2;
      } else {
        const uint8_t cur = rowPtr[bytePos];
        if (hi) {
          rowPtr[bytePos] = (uint8_t)((paletteIdx & 0x0F) << 4 | (cur & 0x0F));
        } else {
          rowPtr[bytePos] = (uint8_t)((cur & 0xF0) | (paletteIdx & 0x0F));
        }
        xPx += 1;
      }
    }
  }
}

const char* g2ProbeImageQ9FrameBuilder() {
  EXT_RAM_BSS_ATTR static char ret[220];

  G2Temple* arm = pickEvenAiArm("imgQ9");
  if (!arm) return "Img Q9: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic   = allocCreateMagic();
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x17;  // 231..237
  const int32_t  kImgW          = 288;
  const int32_t  kImgH          = 144;

  probeBanner("Q9: frame builder (3-band composed BMP)",
              kCreateMagic, kPushMagicBase, kPushMagicBase + 7,
              "builds a 288×144 BMP from rect primitives (white header, "
              "stripe body, black footer). Validates the draw API we'll "
              "use for real feature content.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q9: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    return "Img Q9: BMP heap alloc failed";
  }
  // Start from the stripes baseline (so the body band has stripes for
  // free), then overlay header/footer rects with bmpDrawRect4bpp.
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(cleanupFence);
    return "Img Q9: BMP build failed";
  }
  // Pixel rows live after the file header (14 B) + DIB header (40 B) +
  // 16-entry palette (16*4=64 B) = offset 118. buildBmp4bpp's bfOffBits
  // field is at offset 10 of the file header, so we could read that;
  // for the known layout we hardcode 118.
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  uint8_t* rows = bmp + kPixOffset;
  bmpDrawRect4bpp(rows, kImgW, -kImgH, /*x*/0, /*y*/0,   /*w*/kImgW, /*h*/24,  /*idx*/15);
  bmpDrawRect4bpp(rows, kImgW, -kImgH, /*x*/0, /*y*/120, /*w*/kImgW, /*h*/24,  /*idx*/0);

  unsigned okFrags = 0;
  unsigned totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q9", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q9 — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q9 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(cleanupFence);
  probeFooter("Q9", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q9: %u/%u frags (3-band frame), hold ended via %s",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q11 — minimal streaming swap. CREATE once, push frame A, wait, push
// frame B (different content) into the same container. If B replaces
// A cleanly, we have a streaming pipeline; per-frame cost drops from
// ~3 s (with re-CREATE) to ~2.5 s (push-only).
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ11SimpleSwap() {
  EXT_RAM_BSS_ATTR static char ret[240];

  G2Temple* arm = pickEvenAiArm("imgQ11");
  if (!arm) return "Img Q11: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  // CREATE correlation rotates; frame A uses Cmd=3 band 0x19..0x1F and
  // frame B uses 0x20..0x26 (7 magics each).
  const uint32_t kCreateMagic    = allocCreateMagic();
  const uint32_t kPushAMagicBase = G2_MAGIC_IMAGE_BASE + 0x19;  // 233..239
  const uint32_t kPushBMagicBase = G2_MAGIC_IMAGE_BASE + 0x20;  // 242..248
  const int32_t  kImgW           = 288;
  const int32_t  kImgH           = 144;

  probeBanner("Q11: simple swap (push A → wait → push B, no re-CREATE)",
              kCreateMagic, kPushAMagicBase, kPushBMagicBase + 7,
              "expect Cmd=1 res=0 then 7 cmd=4 acks for A, ~3s hold, "
              "then 7 more cmd=4 acks for B. If B's content replaces "
              "A on the lens, the firmware accepts back-to-back "
              "Cmd=3 sessions on a single CREATE — streaming works.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q11: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmpA");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmpB");
  if (!bmpA || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(cleanupFence);
    return "Img Q11: BMP heap alloc failed";
  }
  const size_t bmpALen = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  // Frame B: stripes baseline, then a black header band so it's
  // visually distinct from A. (No primitives lib in flight — reuse Q9's
  // helper.)
  const size_t bmpBLen = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpALen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpB);
    probePostProbeShutdown(cleanupFence);
    return "Img Q11: BMP build failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  bmpDrawRect4bpp(bmpB + kPixOffset, kImgW, -kImgH,
                  /*x*/0, /*y*/0, /*w*/kImgW, /*h*/72, /*idx*/0);

  // Frame A: full CREATE + push.
  unsigned okA = 0, totalA = 0;
  G2ImageCreateFence createFence = {};
  (void)sendImageBmpMultiFragment(*arm, "Q11/A", kCreateMagic, kPushAMagicBase,
                                  bmpA, bmpALen, &okA, &totalA,
                                  /*tolerateMissedAcks*/ 0,
                                  /*imgW*/ 288, /*imgH*/ 144,
                                  G2_IMG_COMPRESS_RAW, &createFence);
  DEBUG_G2F("[ImgProbe] Q11 — frame A shipped (%u/%u), holding 3 s before B",
            okA, totalA);
  vTaskDelay(pdMS_TO_TICKS(3000));

  // Frame B: push only, same container.
  unsigned okB = 0, totalB = 0;
  (void)sendImageBmpFragmentsNoCreate(*arm, "Q11/B", kPushBMagicBase,
                                       /*cid*/ 2, /*cname*/ "imgQ4",
                                       bmpB, bmpBLen, &okB, &totalB,
                                       /*tolerateMissedAcks*/ 0,
                                       /*inFlightCapOverride*/ 0,
                                       G2_IMG_COMPRESS_RAW, nullptr,
                                       createFence.generation,
                                       /*markExpectedEcho*/ false,
                                       createFence.lifecycleEpoch);
  free(bmpA); free(bmpB);

  DEBUG_G2F("[ImgProbe] Q11 — both frames shipped, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q11 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(createFence.arm ? createFence : cleanupFence);
  probeFooter("Q11", okA + okB, totalA + totalB);
  snprintf(ret, sizeof(ret),
           "Img Q11: A=%u/%u, B=%u/%u, hold via %s — if B replaced A, "
           "streaming works",
           okA, totalA, okB, totalB,
           tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q10 — streaming with intermediate black-clear. CREATE → push A →
// wait → push all-black → wait → push B. Tests whether an explicit
// black frame between content frames helps clean transitions; only
// meaningful if Q11 leaves visible artifacts.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ10ClearThenPush() {
  EXT_RAM_BSS_ATTR static char ret[260];

  G2Temple* arm = pickEvenAiArm("imgQ10");
  if (!arm) return "Img Q10: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  // All magics must fit in uint8 ≤ 255. CREATE rotates independently;
  // the three Cmd=3 bands below need 21 contiguous push slots.
  // Confirmed 2026-04-27: a previous Q10 allocation that crossed magic
  // 256 had every push >255 silently dropped — only frags with magic
  // 251..255 acked, the rest never reached the lens. Pack the 21 push slots
  // tightly into 227..247.
  const uint32_t kCreateMagic       = allocCreateMagic();
  const uint32_t kPushAMagicBase    = G2_MAGIC_IMAGE_BASE + 0x11;  // 227..233
  const uint32_t kPushBlackMagicBase= G2_MAGIC_IMAGE_BASE + 0x18;  // 234..240
  const uint32_t kPushBMagicBase    = G2_MAGIC_IMAGE_BASE + 0x1F;  // 241..247
  const int32_t  kImgW              = 288;
  const int32_t  kImgH              = 144;

  probeBanner("Q10: clear-then-push (A → black → B, no re-CREATE)",
              kCreateMagic, kPushAMagicBase, kPushBMagicBase + 7,
              "tests whether an intermediate black frame between A and "
              "B improves transition quality. Only meaningful if Q11 "
              "shows tearing or ghosting.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q10: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmpA");
  uint8_t* bmpBlack = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmpBlack");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmpB");
  if (!bmpA || !bmpBlack || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpBlack) free(bmpBlack);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(cleanupFence);
    return "Img Q10: BMP heap alloc failed";
  }
  const size_t bmpALen     = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  const size_t bmpBlackLen = buildBmp4bpp(bmpBlack, kBmpCap, kImgW, -kImgH, BMP_PAT_ALL_BLACK);
  const size_t bmpBLen     = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpALen == 0 || bmpBlackLen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpBlack); free(bmpB);
    probePostProbeShutdown(cleanupFence);
    return "Img Q10: BMP build failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  bmpDrawRect4bpp(bmpB + kPixOffset, kImgW, -kImgH,
                  /*x*/0, /*y*/0, /*w*/kImgW, /*h*/72, /*idx*/0);

  unsigned okA = 0, totalA = 0;
  G2ImageCreateFence createFence = {};
  (void)sendImageBmpMultiFragment(*arm, "Q10/A", kCreateMagic, kPushAMagicBase,
                                  bmpA, bmpALen, &okA, &totalA,
                                  /*tolerateMissedAcks*/ 0,
                                  /*imgW*/ 288, /*imgH*/ 144,
                                  G2_IMG_COMPRESS_RAW, &createFence);
  DEBUG_G2F("[ImgProbe] Q10 — A shipped (%u/%u), 2 s hold then black", okA, totalA);
  vTaskDelay(pdMS_TO_TICKS(2000));

  unsigned okBlack = 0, totalBlack = 0;
  (void)sendImageBmpFragmentsNoCreate(*arm, "Q10/black", kPushBlackMagicBase,
                                       /*cid*/ 2, /*cname*/ "imgQ4",
                                       bmpBlack, bmpBlackLen, &okBlack, &totalBlack,
                                       /*tolerateMissedAcks*/ 0,
                                       /*inFlightCapOverride*/ 0,
                                       G2_IMG_COMPRESS_RAW, nullptr,
                                       createFence.generation,
                                       /*markExpectedEcho*/ false,
                                       createFence.lifecycleEpoch);
  DEBUG_G2F("[ImgProbe] Q10 — black shipped (%u/%u), 1 s hold then B", okBlack, totalBlack);
  vTaskDelay(pdMS_TO_TICKS(1000));

  unsigned okB = 0, totalB = 0;
  (void)sendImageBmpFragmentsNoCreate(*arm, "Q10/B", kPushBMagicBase,
                                       /*cid*/ 2, /*cname*/ "imgQ4",
                                       bmpB, bmpBLen, &okB, &totalB,
                                       /*tolerateMissedAcks*/ 0,
                                       /*inFlightCapOverride*/ 0,
                                       G2_IMG_COMPRESS_RAW, nullptr,
                                       createFence.generation,
                                       /*markExpectedEcho*/ false,
                                       createFence.lifecycleEpoch);
  free(bmpA); free(bmpBlack); free(bmpB);

  DEBUG_G2F("[ImgProbe] Q10 — all 3 frames shipped, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q10 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(createFence.arm ? createFence : cleanupFence);
  probeFooter("Q10", okA + okBlack + okB, totalA + totalBlack + totalB);
  snprintf(ret, sizeof(ret),
           "Img Q10: A=%u/%u, black=%u/%u, B=%u/%u, hold via %s",
           okA, totalA, okBlack, totalBlack, okB, totalB,
           tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// QGlizzy — static-image probe with a hardcoded path. Loads
// /sd/PICTURES/test.bmp from VFS, validates it through readBmpFromVfs
// (BM header, 4bpp, 288×144), and pushes via the standard
// sendImageBmpMultiFragment helper. Same transport as Q6/g2bmp; the
// only difference is no operator input — useful as a quick static-
// image canary on the Static Tests sub-menu.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQGlizzy() {
  EXT_RAM_BSS_ATTR static char ret[220];

  G2Temple* arm = pickEvenAiArm("imgQGlizzy");
  if (!arm) return "Img QGlizzy: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  // 7 push frags max for a 288×144 4bpp BMP. Cmd=3 uses 239..245,
  // comfortably ≤ 255; CREATE correlation rotates independently.
  const uint32_t kCreateMagic   = allocCreateMagic();
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x1D;  // 239..245

  probeBanner("QGlizzy: load /sd/PICTURES/test.bmp and push",
              kCreateMagic, kPushMagicBase, kPushMagicBase + 7,
              "loads a 288×144 4bpp BMP from SD and ships it through "
              "the same pipeline as Q6/g2bmp. Hold until double-tap "
              "(60 s safety cap). Useful canary for SD-backed image "
              "delivery — no operator input required.");

  const char* loadErr = "";
  uint8_t* bmp = nullptr;
  size_t bmpLen = 0;
  int32_t bmpW = 0;
  int32_t bmpH = 0;
  if (!readBmpFromVfs(String("/sd/PICTURES/test.bmp"),
                      &bmp, &bmpLen, &bmpW, &bmpH, &loadErr)) {
    DEBUG_G2F("[ImgProbe] QGlizzy — load failed: %s", loadErr ? loadErr : "?");
    snprintf(ret, sizeof(ret),
             "Img QGlizzy: load failed — %s", loadErr ? loadErr : "unknown");
    return ret;
  }
  DEBUG_G2F("[ImgProbe] QGlizzy loaded: %u B (%dx%d)",
            (unsigned)bmpLen, (int)bmpW, (int)bmpH);

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    free(bmp);
    return "Img QGlizzy: pre-burst SHUTDOWN failed";
  }

  unsigned okFrags = 0;
  unsigned totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "QGlizzy",
                                  kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  free(bmp);

  DEBUG_G2F("[ImgProbe] QGlizzy — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] QGlizzy — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(cleanupFence);
  probeFooter("QGlizzy", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img QGlizzy: %u/%u frags from /sd/PICTURES/test.bmp, hold via %s",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q12 — full-display 576×288 image as 2×2 grid of 288×144 tiles.
// Single CREATE declares all 4 ImageObject children at distinct
// positions/CIDs; subsequent Cmd=3 pushes target each tile by its
// CID/name pair. Each tile carries the same striped baseline plus a
// 24×24 white square at the corner facing the display centre — when
// alignment is correct the four squares form a single 48×48 white
// block at the dead centre of the lens.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ12FullScreen() {
  EXT_RAM_BSS_ATTR static char ret[260];

  G2Temple* arm = pickEvenAiArm("imgQ12");
  if (!arm) return "Img Q12: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  // Four Cmd=3 tile bands occupy 211..238. CREATE correlation uses the
  // independent shared rotating low-byte sequence.
  const uint32_t kCreateMagic    = allocCreateMagic();
  const uint32_t kPushBase[4]    = {
      G2_MAGIC_IMAGE_BASE + 0x01,  // 211..217  TL
      G2_MAGIC_IMAGE_BASE + 0x08,  // 218..224  TR
      G2_MAGIC_IMAGE_BASE + 0x0F,  // 225..231  BL
      G2_MAGIC_IMAGE_BASE + 0x16,  // 232..238  BR
  };
  const int32_t  kTileW          = 288;
  const int32_t  kTileH          = 144;
  const int32_t  kFullW          = 576;
  const int32_t  kFullH          = 288;
  (void)kFullW; (void)kFullH;  // documented for the operator; not sent on wire

  // Tile descriptors — declared in the single CREATE, referenced
  // individually by subsequent Cmd=3 pushes.
  const G2ImageTile kTiles[4] = {
    { /*x*/ 0,                       /*y*/ 0,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 2, /*name*/ "tileTL" },
    { /*x*/ (uint32_t)kTileW,        /*y*/ 0,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 3, /*name*/ "tileTR" },
    { /*x*/ 0,                       /*y*/ (uint32_t)kTileH,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 4, /*name*/ "tileBL" },
    { /*x*/ (uint32_t)kTileW,        /*y*/ (uint32_t)kTileH,
      /*w*/ (uint32_t)kTileW,        /*h*/ (uint32_t)kTileH,
      /*cid*/ 5, /*name*/ "tileBR" },
  };

  probeBanner("Q12: full-screen 576×288 (2×2 grid of 288×144 tiles)",
              kCreateMagic, kPushBase[0], kPushBase[3] + 7,
              "single CREATE with 4 ImageObject children, then 4 "
              "sequential Cmd=3 streams (one per tile). When alignment "
              "is correct the four 24×24 corner squares converge to a "
              "single 48×48 white block at lens centre.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q12: pre-burst SHUTDOWN failed";

  // Build + send the multi-tile CREATE.
  uint8_t createBuf[256];
  size_t createLen = g2BuildCreateImageMulti(
      allocSeq(), kCreateMagic, kTiles, /*tileCount*/ 4,
      BLOCKS_WIDGET_ID, createBuf, sizeof(createBuf));
  if (createLen == 0) return "Img Q12: CREATE-multi build failed";

  noteOurShutdownSent();
  DEBUG_G2F("[ImgProbe] Q12 CREATE-multi magic=%u (4 tiles, full %dx%d, widgetId=%u)",
            (unsigned)kCreateMagic, (int)kFullW, (int)kFullH,
            (unsigned)BLOCKS_WIDGET_ID);
  logPbHex("Q12 env", createBuf, createLen);
  if (!probePrepImageCreateAck(kCreateMagic, arm)) {
    clearOurShutdownStamp();
    return "Img Q12: CREATE rejected by lifecycle fence";
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    clearOurShutdownStamp();
    return "Img Q12: CREATE fence capture failed";
  }
  if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
    g2CreateTxnRelease();
    probePostProbeShutdown(createFence);
    return "Img Q12: CREATE-multi TX failed";
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs,
                               arm, createFence.generation)) {
    probePostProbeShutdown(createFence);
    return "Img Q12: CREATE-multi ack timeout (4-tile geometry rejected?)";
  }
  DEBUG_G2F("[ImgProbe] Q12 CREATE-multi acked — pushing 4 tiles");

  // Single repaint buffer reused across all 4 tile pushes.
  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(createFence);
    return "Img Q12: BMP heap alloc failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;

  // Per-tile inside-corner anchors. The corner that faces the centre
  // of the full display, in the tile's own (288×144) coordinate space.
  struct CornerXY { int32_t x, y; };
  const CornerXY kCorner[4] = {
    { kTileW - 24, kTileH - 24 },  // TL → bottom-right of tile
    { 0,           kTileH - 24 },  // TR → bottom-left of tile
    { kTileW - 24, 0           },  // BL → top-right of tile
    { 0,           0           },  // BR → top-left of tile
  };
  const char* kTileTag[4] = { "Q12/TL", "Q12/TR", "Q12/BL", "Q12/BR" };

  unsigned okTotal = 0, fragTotal = 0;
  for (size_t i = 0; i < 4; i++) {
    size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kTileW, -kTileH, BMP_PAT_STRIPES);
    if (bmpLen == 0) {
      free(bmp);
      probePostProbeShutdown(createFence);
      return "Img Q12: BMP build failed";
    }
    bmpDrawRect4bpp(bmp + kPixOffset, kTileW, -kTileH,
                    kCorner[i].x, kCorner[i].y, /*w*/ 24, /*h*/ 24, /*idx*/ 15);

    unsigned ok = 0, total = 0;
    (void)sendImageBmpFragmentsNoCreate(*arm, kTileTag[i], kPushBase[i],
                                         /*cid*/ kTiles[i].containerId,
                                         /*cname*/ kTiles[i].containerName,
                                         bmp, bmpLen, &ok, &total,
                                         /*tolerateMissedAcks*/ 0,
                                         /*inFlightCapOverride*/ 0,
                                         G2_IMG_COMPRESS_RAW, nullptr,
                                         createFence.generation,
                                         /*markExpectedEcho*/ false,
                                         createFence.lifecycleEpoch);
    okTotal   += ok;
    fragTotal += total;
    DEBUG_G2F("[ImgProbe] Q12 tile %u/4 (%s) shipped %u/%u",
              (unsigned)(i + 1), kTileTag[i], ok, total);
    // Brief inter-tile gap so the firmware's reassembly window for tile N
    // drains before tile N+1 starts piling up. 50 ms after each tile at the
    // current per-tile burst baseline (~2.2 s); increase if multi-tile probes
    // show cross-tile reassembly issues.
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q12 — full-display image up (%u/%u total frags), "
            "double-tap to dismiss (60 s cap)",
            okTotal, fragTotal);
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q12 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(createFence);
  probeFooter("Q12", okTotal, fragTotal);
  snprintf(ret, sizeof(ret),
           "Img Q12: 4-tile full-screen %u/%u frags, hold via %s",
           okTotal, fragTotal, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q13 — live image tile pipeline. Single 288×144 container; each tick
// rasterises a fresh BMP (stripes pattern with a moving horizontal bar
// indexed by frame counter) and pushes it without re-CREATE. Loops
// until the user double-taps or a safety cap fires. Tick cadence is
// driven by `gG2LiveRateMs` (CLI: `g2liverate`); achievable cadence is
// gated by per-push burst time (~2.6 s per 288×144 frame on this stack).
// Logs the requested vs measured cadence per frame so the operator can
// characterise sustained throughput.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ13LiveTile() {
  EXT_RAM_BSS_ATTR static char ret[260];

  G2Temple* arm = pickEvenAiArm("imgQ13");
  if (!arm) return "Img Q13: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kFirstPushBase   = G2_MAGIC_IMAGE_BASE + 0x01;  // 211
  const int32_t  kImgW            = 288;
  const int32_t  kImgH            = 144;
  // Capped at 30 s / 12 frames. Each frame's BMP push hits the BLE
  // throughput limit at ~2.7 s (21 KB BMP + acks); the firmware's own
  // lens-idle timeout is ~30 s. Q13 was previously capped at 2 min /
  // 240 frames, which (a) deceived operators about achievable cadence
  // and (b) ran the hijack past the firmware's natural idle window,
  // forcing us to fight the 60 s hijack-safety watchdog. 30 s is the
  // honest envelope for "sustained streaming" on this BLE link;
  // double-tap dismiss is still available for early exit.
  const uint32_t kSafetyCapMs     = 30000;
  const uint32_t kSafetyCapFrames = 12;

  probeBanner("Q13: live image tile (push @ rate, no re-CREATE)",
              0, kFirstPushBase, kFirstPushBase + 6,
              "single 288x144 container, push a fresh BMP every "
              "g2liverate ms (default 600). Each frame shifts a "
              "horizontal bar to make the update visible. Double-tap "
              "to dismiss; auto-stops at 30 s or 12 frames.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q13: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    return "Img Q13: BMP heap alloc failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;

  // Arm the tap-hold sentinel so the SysEvent handler will set
  // gImgProbeHoldTapPending when the user double-taps. We poll it
  // between frames rather than blocking on probeHoldUntilTapOrTimeout.
  // gImgProbeAbort is the same dismiss signal observed by the inner
  // burst senders — clear it here so a stale value from a prior probe
  // doesn't terminate this run on iter 1.
  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  // Wrap push-magic base across frames so we never exceed uint8.
  // 7 magics per push; wrap to 211 if next 7 would cross 255.
  uint32_t pushMagicBase = kFirstPushBase;
  bool createdOnce = false;
  G2ImageCreateFence createFence = {};
  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  unsigned recreateCount = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;
  bool lensTimedOut = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) { dismissed = true; break; }

    // Lens-idle detection. The firmware fires DISPLAY_OFF after ~30 s
    // of no ring/temple input and our event handler clears
    // containerReady. Without this check the loop happily blasts
    // image fragments into a dead container until the safety cap.
    if (createdOnce && !arm->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] Q13 lens idle-timeout (recreate #%u)",
                  recreateCount);
        createdOnce = false;  // next push goes through CREATE path
      } else {
        lensTimedOut = true;
        break;
      }
    }

    // Build BMP for this frame. Bar is 12 px tall and walks down by
    // 12 px per frame, wrapping at the bottom.
    size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
    if (bmpLen == 0) break;
    const int32_t barY = (int32_t)((framesAttempted * 12) % (uint32_t)(kImgH - 12));
    bmpDrawRect4bpp(bmp + kPixOffset, kImgW, -kImgH,
                    /*x*/ 0, /*y*/ barY, /*w*/ kImgW, /*h*/ 12, /*idx*/ 15);

    const unsigned fragsThisFrame =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    // Wrap push-magic window if the next burst would cross uint8.
    if (pushMagicBase + fragsThisFrame > 256) pushMagicBase = kFirstPushBase;

    const uint32_t frameStartMs = millis();
    bool ok = false;
    if (!createdOnce) {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpMultiFragment(*arm, "Q13",
                                     allocCreateMagic(), pushMagicBase,
                                     bmp, bmpLen, &okFrags, &totalFrags,
                                     /*tolerateMissedAcks*/ 0,
                                     /*imgW*/ 288, /*imgH*/ 144,
                                     G2_IMG_COMPRESS_RAW, &createFence);
      createdOnce = ok;
      // Stamp lens/arm container state on first successful CREATE-image so the
      // lens-idle gate above doesn't trip on iter 2. sendImageBmpMultiFragment
      // doesn't do this itself — by contract (sendCreateAndWait note) the
      // caller is responsible. Without this, Q13 bails as "lens timeout"
      // after a single frame even though the container is alive.
      if (ok) {
        arm->containerReady  = true;
        arm->containerIsList = false;
        g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);
      }
    } else {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpFragmentsNoCreate(*arm, "Q13",
                                          pushMagicBase,
                                          /*cid*/ 2, /*cname*/ "imgQ4",
                                          bmp, bmpLen, &okFrags, &totalFrags,
                                          /*tolerateMissedAcks*/ 0,
                                          /*inFlightCapOverride*/ 0,
                                          G2_IMG_COMPRESS_RAW, nullptr,
                                          createFence.generation,
                                          /*markExpectedEcho*/ false,
                                          createFence.lifecycleEpoch);
    }
    pushMagicBase += fragsThisFrame;
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] Q13 frame %u: %s in %u ms (req=%u ms)",
              framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)gG2LiveRateMs);

    // Sleep to honour the requested cadence, but skip the sleep if the
    // frame already took longer than the rate. Poll the tap sentinel
    // during the sleep so dismiss is responsive.
    if (frameMs < gG2LiveRateMs) {
      const uint32_t sleepMs = gG2LiveRateMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) { dismissed = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;
  free(bmp);

  const char* reason = dismissed     ? "user tap"
                     : lensTimedOut  ? "lens timeout"
                                     : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q13 ended (%s): %u/%u frames %u recreates in %u ms "
            "(avg %u ms/frame)",
            reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  probePostProbeShutdown(createFence.arm ? createFence : cleanupFence);
  probeFooter("Q13", framesOk, framesAttempted);
  snprintf(ret, sizeof(ret),
           "Img Q13: %u/%u frames in %u ms, %u re-creates, end via %s",
           framesOk, framesAttempted, (unsigned)totalMs,
           recreateCount, reason);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q14 — live TEXT REBUILD pipeline. Uses g2ShowText which CREATEs once
// then issues Cmd=7 REBUILD_PAGE on subsequent calls (~one envelope per
// update vs Q13's ~7-fragment multi-push). Each tick fires a new
// content string with frame counter + uptime so the operator can see
// the surface ticking. Cadence governed by gG2LiveRateMs.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ14LiveText() {
  EXT_RAM_BSS_ATTR static char ret[260];

  if (!gR.connected && !gL.connected) {
    return "Img Q14: no reachable temple";
  }

  // Capped at 30 s / 60 frames — see Q13 rationale. Q14 is text-only
  // (REBUILD per tick), so each "frame" is much cheaper than Q13's full
  // BMP push, but the firmware's lens-idle window is the same ~30 s, so
  // running longer is still fighting natural lens lifecycle. Operators
  // wanting sustained text-on-lens benches can re-arm via the test menu.
  const uint32_t kSafetyCapMs     = 30000;
  const uint32_t kSafetyCapFrames = 60;

  probeBanner("Q14: live TEXT (REBUILD @ rate)",
              /*createMagic*/ 0, G2_MAGIC_REBUILD, G2_MAGIC_REBUILD,
              "g2ShowText() loop. First call CREATEs the TEXT widget, "
              "subsequent calls REBUILD_PAGE — single envelope per "
              "update. Cadence via g2liverate (default 600 ms). "
              "Double-tap to dismiss; auto-stops at 2 min or 1000 frames.");

  // Tear down the hijack list before the loop. Without this, g2ShowText
  // sees the live container is a List widget (the test-suite menu) and
  // auto-routes to g2ShowTextAsList, which does SHUTDOWN+CREATE-list
  // every frame — visible flicker. Tearing down here lets the first
  // g2ShowText below CREATE a fresh TEXT widget; subsequent calls then
  // hit the Cmd=7 REBUILD_PAGE fast path with no visible teardown.
  G2Temple* armPre = pickEvenAiArm("imgQ14");
  if (!armPre) return "Img Q14: no reachable temple";
  G2ImageCreateFence cleanupFence = {};
  if (!probeTearDownActiveContainer(*armPre, &cleanupFence)) {
    return "Img Q14: pre-loop SHUTDOWN failed";
  }

  // Same dismiss-via-double-tap pattern as Q13. The TEXT widget on
  // 2.2.0.24 ALSO emits DOUBLE_CLICK_EVENT(3) src=2 from the ring per
  // the protocol doc, so the existing tap-hold sentinel works for
  // both image and text surfaces.
  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  unsigned recreateCount = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;
  bool lensTimedOut = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) { dismissed = true; break; }

    // Lens-idle detection — same gate as Q13. g2ShowText's first call
    // CREATEs the TEXT widget, subsequent calls REBUILD. When DISPLAY_OFF
    // fires the firmware tears the widget down and our event handler
    // clears containerReady; the next g2ShowText auto-CREATEs a fresh
    // widget. With keep-alive off, we break here so the loop stops cleanly
    // instead of letting g2ShowText churn through the recreate cycle.
    G2Temple* armCheck = pickEvenAiArm("imgQ14");
    if (framesAttempted > 0 && armCheck && !armCheck->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] Q14 lens idle-timeout (auto-recreate #%u)",
                  recreateCount);
        // No state to clear — g2ShowText handles re-CREATE itself on the
        // next call when containerReady is false.
      } else {
        lensTimedOut = true;
        break;
      }
    }

    char line[96];
    const uint32_t upS = (millis() - startMs) / 1000u;
    snprintf(line, sizeof(line),
             "Live #%u\nup=%u s\nrate=%u ms",
             framesAttempted + 1, (unsigned)upS, (unsigned)gG2LiveRateMs);

    const uint32_t frameStartMs = millis();
    const bool ok = g2ShowText(line);
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] Q14 frame %u: %s in %u ms (req=%u ms)",
              framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)gG2LiveRateMs);

    if (frameMs < gG2LiveRateMs) {
      const uint32_t sleepMs = gG2LiveRateMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) { dismissed = true; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;

  const char* reason = dismissed     ? "user tap"
                     : lensTimedOut  ? "lens timeout"
                                     : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q14 ended (%s): %u/%u rebuilds %u recreates in %u ms "
            "(avg %u ms/frame)",
            reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  // Q14 uses a TEXT widget — clean it up via the standard hijack path
  // so the next probe / picker rebuild starts from a known state.
  probePostProbeShutdown(cleanupFence);
  probeFooter("Q14", framesOk, framesAttempted);
  snprintf(ret, sizeof(ret),
           "Img Q14: %u/%u rebuilds in %u ms, %u re-creates, end via %s",
           framesOk, framesAttempted, (unsigned)totalMs,
           recreateCount, reason);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q15 — left-arm image push test. Same BMP and transport as Q6 (single
// 288×144 tile, 7-fragment burst), but explicitly targets the LEFT
// temple instead of letting pickEvenAiArm pick the RIGHT default.
//
// Motivation: 2026-04-28 third-party report (jimrandomh on Discord)
// claimed "significant performance improvement if you send image
// fragments to the left arm instead of the right." Test framework:
// run Q6 (or Q15 paired) for the right-arm baseline (~2.6 s), then
// Q15 for the left-arm timing, and compare. If left is meaningfully
// faster (e.g. <2.0 s), that's an opportunity to change the default
// arm-picker for image-push helpers.
//
// Caveats observed in our setup:
//   * On firmware 2.2.0.24 and 2.2.7.14, ALL notify traffic (incl. cmd=4
//     ImageRawResp acks) arrives on the RIGHT pipe — see docs/G2_PROTOCOL.md
//     "Notify channel topology — right-only". So even when we TX to LEFT,
//     acks come back via RIGHT and the existing ack-tracking path still works.
//   * Because L never receives heartbeat acks on these firmware builds, beatOne()
//     skips the miss-counter for L (firmwareRoutesEvenHubRepliesToRight gate),
//     keeping gL.pluginDead=false in steady state. The pluginDead
//     check below remains as a safety net for the case where some
//     other code path (e.g. an explicit teardown) sets it.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ15LeftArm() {
  EXT_RAM_BSS_ATTR static char ret[260];

  if (!gL.connected) {
    return "Img Q15: LEFT temple not connected";
  }
  if (gL.pluginDead) {
    return "Img Q15: LEFT temple plugin silent — heartbeat acks not arriving; "
           "wear the glasses or wake them, then retry";
  }
  G2Temple* arm = &gL;
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic    = allocCreateMagic();
  const uint32_t kPushMagicBase  = G2_MAGIC_IMAGE_BASE + 0x01;  // 211..217
  const int32_t  kImgW           = 288;
  const int32_t  kImgH           = 144;
  const size_t   kChunkBytes     = G2_IMG_MAPRAW_CHUNK_BYTES;

  probeBanner("Q15: LEFT-arm image push (288x144, baseline-vs-LEFT test)",
              kCreateMagic, kPushMagicBase, kPushMagicBase + 7,
              "same BMP / transport / fragmentation as Q6 but TXed via "
              "LEFT temple. Compare burst time to Q6's ~2.6 s baseline. "
              "Per Discord 2026-04-28 left may be faster — verify.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q15: pre-burst SHUTDOWN failed";

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    return "Img Q15: BMP heap alloc failed";
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(cleanupFence);
    return "Img Q15: BMP build failed";
  }
  const unsigned kFrags = (unsigned)((bmpLen + kChunkBytes - 1) / kChunkBytes);
  DEBUG_G2F("[ImgProbe] Q15 LEFT-arm: BMP %u B (%dx%d 4bpp), %u chunks of <=%u B",
            (unsigned)bmpLen, kImgW, kImgH, kFrags, (unsigned)kChunkBytes);

  unsigned okFrags = 0, totalFrags = 0;
  const uint32_t burstStartMs = millis();
  (void)sendImageBmpMultiFragment(*arm, "Q15/L",
                                  kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags);
  const uint32_t burstMs = millis() - burstStartMs;
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q15 LEFT-arm result: %u/%u frags in %u ms "
            "(Q6 right-arm baseline ~2.6 s — left %s)",
            okFrags, totalFrags, (unsigned)burstMs,
            burstMs < 2400 ? "FASTER" :
            burstMs > 2800 ? "SLOWER" : "comparable");

  DEBUG_G2F("[ImgProbe] Q15 — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q15 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(cleanupFence);
  probeFooter("Q15", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q15: LEFT-arm %u/%u frags in %u ms (vs Q6 right ~2600 ms)",
           okFrags, totalFrags, (unsigned)burstMs);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q16/Q17/Q18 — mixed CREATE (List + Image) composition probes.
// Tests whether the firmware accepts multi-type widget declarations in
// a single CreateStartUpPageContainer (schema-allowed but never tested
// in this codebase before 2026-04-28). Each probe varies the list and
// image geometry to characterise:
//   Q16: side-by-side, no overlap — does mixed CREATE ack at all?
//   Q17: image overlapping middle of list — what's the z-order?
//   Q18: small image in corner of full-screen list — does the firmware
//        accept non-288×144 image containers?
// ─────────────────────────────────────────────────────────────────────

// Shared helper — does the heavy lifting for all three probes. Caller
// passes the geometry and image dimensions; helper handles CREATE+ack
// wait, BMP build+push, hold-for-tap, teardown.
static const char* runMixedListImageProbe(const char* tag,
                                          const G2ContainerGeom& listGeom,
                                          int32_t imgX, int32_t imgY,
                                          int32_t imgW, int32_t imgH,
                                          char* retBuf, size_t retCap) {
  G2Temple* arm = pickEvenAiArm(tag);
  if (!arm) {
    snprintf(retBuf, retCap, "%s: no reachable temple", tag);
    return retBuf;
  }
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic    = allocCreateMagic();
  const uint32_t kPushMagicBase  = G2_MAGIC_IMAGE_BASE + 0x11;  // 227..233
  const uint32_t kListCid        = 1;
  const char*    kListName       = "mixL";
  const uint32_t kImageCid       = 2;
  const char*    kImageName      = "mixI";

  // Concise list payload — keep it short so the CREATE envelope stays
  // under the builder's 1 KB payload budget. 5 rows is plenty to verify
  // the list renders.
  const char* listItems[] = {
    "<- Back",
    "Mixed CREATE",
    "list+image test",
    "row 4",
    "row 5",
  };
  const size_t listItemCount = sizeof(listItems) / sizeof(listItems[0]);

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(retBuf, retCap, "%s: pre-burst SHUTDOWN failed", tag);
    return retBuf;
  }

  // Build and send the mixed CREATE.
  G2ImageTile imgTile;
  imgTile.x = (uint32_t)imgX;
  imgTile.y = (uint32_t)imgY;
  imgTile.w = (uint32_t)imgW;
  imgTile.h = (uint32_t)imgH;
  imgTile.containerId = kImageCid;
  imgTile.containerName = kImageName;

  uint8_t createBuf[1024];
  size_t createLen = g2BuildCreateMixedListImage(
      allocSeq(), kCreateMagic,
      kListName, listItems, listItemCount, listGeom,
      imgTile, BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(retBuf, retCap, "%s: CREATE-mixed build failed", tag);
    return retBuf;
  }

  DEBUG_G2F("[ImgProbe] %s CREATE-mixed magic=%u "
            "(list@(%u,%u,%u,%u) image@(%d,%d,%dx%d))",
            tag, (unsigned)kCreateMagic,
            (unsigned)listGeom.x, (unsigned)listGeom.y,
            (unsigned)listGeom.w, (unsigned)listGeom.h,
            (int)imgX, (int)imgY, (int)imgW, (int)imgH);
  logPbHex("CREATE-mixed env", createBuf, createLen);

  if (!probePrepImageCreateAck(kCreateMagic, arm)) {
    snprintf(retBuf, retCap, "%s: CREATE rejected by lifecycle fence", tag);
    return retBuf;
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    snprintf(retBuf, retCap, "%s: CREATE fence capture failed", tag);
    return retBuf;
  }
  if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
    g2CreateTxnRelease();
    probePostProbeShutdown(createFence);
    snprintf(retBuf, retCap, "%s: CREATE-mixed TX failed", tag);
    return retBuf;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs,
                               arm, createFence.generation)) {
    probePostProbeShutdown(createFence);
    snprintf(retBuf, retCap, "%s: CREATE-mixed ack timeout — firmware "
                             "may not accept list+image composition",
             tag);
    return retBuf;
  }
  DEBUG_G2F("[ImgProbe] %s CREATE-mixed acked — pushing image to '%s' "
            "(CID=%u, %dx%d)",
            tag, kImageName, (unsigned)kImageCid, (int)imgW, (int)imgH);

  // Build and push the BMP. Negative height = top-down for intuitive
  // pattern testing.
  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.bmp");
  if (!bmp) {
    probePostProbeShutdown(createFence);
    snprintf(retBuf, retCap, "%s: BMP heap alloc failed", tag);
    return retBuf;
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, imgW, -imgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(createFence);
    snprintf(retBuf, retCap, "%s: BMP build failed (%dx%d)",
             tag, (int)imgW, (int)imgH);
    return retBuf;
  }
  DEBUG_G2F("[ImgProbe] %s BMP %u B (%dx%d 4bpp)",
            tag, (unsigned)bmpLen, (int)imgW, (int)imgH);

  // Q16/Q17/Q18 stay forced RAW as the uncompressed mixed control.
  // Production / auto path uses bare-block LZ4 (Q16d paints).
  unsigned okFrags = 0, totalFrags = 0;
  {
    G2ImgLz4AutoOffGuard forceRaw;
    (void)sendImageBmpFragmentsNoCreate(*arm, tag, kPushMagicBase,
                                         /*cid*/ kImageCid,
                                         /*cname*/ kImageName,
                                         bmp, bmpLen, &okFrags, &totalFrags,
                                         /*tolerateMissedAcks*/ 0,
                                         /*inFlightCapOverride*/ 0,
                                         G2_IMG_COMPRESS_RAW, nullptr,
                                         createFence.generation,
                                         /*markExpectedEcho*/ false,
                                         createFence.lifecycleEpoch);
  }
  free(bmp);
  DEBUG_G2F("[ImgProbe] %s push complete: %u/%u frags acked",
            tag, okFrags, totalFrags);

  DEBUG_G2F("[ImgProbe] %s — both widgets up; observe lens for "
            "list+image composition. Double-tap to dismiss (60 s cap).",
            tag);
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] %s — hold ended via %s",
            tag, tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(createFence);
  probeFooter(tag, okFrags, totalFrags);
  snprintf(retBuf, retCap,
           "Img %s: CREATE-mixed acked, image %u/%u frags (list@%dx%d, "
           "image@%dx%d) — observe lens for composition",
           tag, okFrags, totalFrags,
           (int)listGeom.w, (int)listGeom.h, (int)imgW, (int)imgH);
  return retBuf;
}

// Q16: side-by-side. List occupies the top half; image occupies the
// bottom half (centered horizontally). No overlap. The most likely-to-
// work shape — if this fails, mixed CREATE is rejected outright.
const char* g2ProbeImageQ16MixedSideBySide() {
  EXT_RAM_BSS_ATTR static char ret[260];
  // List in top half (8..568 wide, 8..138 tall) — leaves room for ~5
  // text rows.
  const G2ContainerGeom listGeom = { 8, 8, 560, 130 };
  // Image in bottom half — 288×144 tile centered horizontally so it
  // sits flush against the bottom edge.
  return runMixedListImageProbe("Q16", listGeom,
                                /*imgX*/ 144, /*imgY*/ 144,
                                /*imgW*/ 288, /*imgH*/ 144,
                                ret, sizeof(ret));
}

// Q16d — H3 from phone app mining: Even Realities app 2.2.7 compresses
// with package:dart_lz4 lz4BlockCompress (bare LZ4 *block*), not frame.
// Solo FRAME_CSIZE paints; mixed FRAME_CSIZE blanks. This probe keeps
// Q16 list-first CREATE + LZ4-of-BMP but uses bare block + cmode=2.
const char* g2ProbeImageQ16dMixedLz4Block() {
  EXT_RAM_BSS_ATTR static char ret[280];
  const char* tag = "Q16d";
  G2Temple* arm = pickEvenAiArm(tag);
  if (!arm) {
    snprintf(ret, sizeof(ret), "%s: no reachable temple", tag);
    return ret;
  }
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic   = allocCreateMagic();
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x11;  // 227..
  const uint32_t kListCid       = 1;
  const char*    kListName      = "mixL";
  const uint32_t kImageCid      = 2;
  const char*    kImageName     = "mixI";
  const int32_t  kImgW          = 288;
  const int32_t  kImgH          = 144;
  const G2ContainerGeom listGeom = { 8, 8, 560, 130 };

  const char* listItems[] = {
    "<- Back",
    "Q16d LZ4 block",
    "dart_lz4 shape",
    "not FRAME_CSIZE",
    "bars = H3 yes",
  };
  const size_t listItemCount = sizeof(listItems) / sizeof(listItems[0]);

  probeBanner("Q16d: mixed CREATE + LZ4 bare-block (Even App dart_lz4)",
              kCreateMagic, kPushMagicBase, kPushMagicBase + 5,
              "Same geom as Q16. Payload is lz4CompressBlock(BMP) with "
              "cmode=2 — no frame magic/csize. Phone app uses "
              "package:dart_lz4 block compress. WATCH LENS: bars under "
              "list = mixed wants block; blank = framing not the fork.");

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(ret, sizeof(ret), "%s: pre-burst SHUTDOWN failed", tag);
    return ret;
  }

  G2ImageTile imgTile;
  imgTile.x = 144;
  imgTile.y = 144;
  imgTile.w = (uint32_t)kImgW;
  imgTile.h = (uint32_t)kImgH;
  imgTile.containerId = kImageCid;
  imgTile.containerName = kImageName;

  uint8_t createBuf[1024];
  size_t createLen = g2BuildCreateMixedListImage(
      allocSeq(), kCreateMagic,
      kListName, listItems, listItemCount, listGeom,
      imgTile, BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(ret, sizeof(ret), "%s: CREATE-mixed build failed", tag);
    return ret;
  }

  DEBUG_G2F("[ImgProbe] %s CREATE-mixed magic=%u "
            "(list@(%u,%u,%u,%u) image@(144,144,%dx%d) payload=LZ4-block)",
            tag, (unsigned)kCreateMagic,
            (unsigned)listGeom.x, (unsigned)listGeom.y,
            (unsigned)listGeom.w, (unsigned)listGeom.h,
            (int)kImgW, (int)kImgH);

  if (!probePrepImageCreateAck(kCreateMagic, arm)) {
    snprintf(ret, sizeof(ret), "%s: CREATE rejected by lifecycle fence", tag);
    return ret;
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    snprintf(ret, sizeof(ret), "%s: CREATE fence capture failed", tag);
    return ret;
  }
  if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
    g2CreateTxnRelease();
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "%s: CREATE-mixed TX failed", tag);
    return ret;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs,
                               arm, createFence.generation)) {
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "%s: CREATE-mixed ack timeout", tag);
    return ret;
  }
  DEBUG_G2F("[ImgProbe] %s CREATE-mixed acked — BMP + LZ4 bare block", tag);

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.q16d.bmp");
  if (!bmp) {
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "%s: BMP alloc failed", tag);
    return ret;
  }
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "%s: BMP build failed", tag);
    return ret;
  }

  const size_t compCap = lz4BlockBound(bmpLen);
  uint8_t* comp = (uint8_t*)ps_alloc(compCap, AllocPref::PreferPsram, "g2.img.q16d.lz4");
  if (!comp) {
    free(bmp);
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "%s: LZ4 alloc failed", tag);
    return ret;
  }
  const uint32_t enc0 = millis();
  const size_t compLen = lz4CompressBlock(bmp, bmpLen, comp, compCap);
  const uint32_t encMs = millis() - enc0;
  free(bmp);
  if (compLen == 0) {
    free(comp);
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "%s: LZ4 block encode failed (%u B BMP)",
             tag, (unsigned)bmpLen);
    return ret;
  }
  DEBUG_G2F("[ImgProbe] %s BMP %u B → LZ4 bare-block %u B (enc %u ms) "
            "— pushing cmode=2 to '%s' (Even App dart_lz4 shape)",
            tag, (unsigned)bmpLen, (unsigned)compLen, (unsigned)encMs,
            kImageName);

  unsigned okFrags = 0, totalFrags = 0;
  {
    G2ImgLz4AutoOffGuard forceNoAuto;
    (void)sendImageBmpFragmentsNoCreate(*arm, tag, kPushMagicBase,
                                         kImageCid, kImageName,
                                         comp, compLen, &okFrags, &totalFrags,
                                         /*tolerateMissedAcks*/ 0,
                                         /*inFlightCapOverride*/ 0,
                                         G2_IMG_COMPRESS_LZ4, nullptr,
                                         createFence.generation,
                                         /*markExpectedEcho*/ false,
                                         createFence.lifecycleEpoch);
  }
  free(comp);
  DEBUG_G2F("[ImgProbe] %s push complete: %u/%u frags acked — observe lens",
            tag, okFrags, totalFrags);

  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] %s — hold ended via %s",
            tag, tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(createFence);
  probeFooter(tag, okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img %s: CREATE-mixed acked, LZ4-block %u/%u frags "
           "(%u→compressed) — did bars paint under list?",
           tag, okFrags, totalFrags, (unsigned)bmpLen);
  return ret;
}

// Q17: image overlapping the right half of a full-screen list. Tests
// whether the firmware paints the image on top of the list (image
// visible, list partly hidden), or under it (list visible, image
// obscured), or rejects the overlap entirely.
//
// 2026-04-27: original geometry centered the image at x=144, which buried
// list text under the tile. Pushed it to x=280 (right edge of the 560-wide
// list area) so the left half stays fully readable while still exercising
// the overlap codepath.
const char* g2ProbeImageQ17MixedOverlap() {
  EXT_RAM_BSS_ATTR static char ret[260];
  // List spans most of the lens (G2_GEOM_LARGE = 8,8,560,272).
  const G2ContainerGeom listGeom = G2_GEOM_LARGE;
  // Image 288×144 tile positioned on the right half of the list:
  // x=280 puts the right edge flush at 568 (matching the list right edge),
  // y=72 keeps it vertically centered. Left ~half of every list row stays
  // visible so the user can read the text and see the overlap behavior.
  return runMixedListImageProbe("Q17", listGeom,
                                /*imgX*/ 280, /*imgY*/ 72,
                                /*imgW*/ 288, /*imgH*/ 144,
                                ret, sizeof(ret));
}

// Q18: small icon-sized image in the top-right corner of a full-screen
// list. Tests whether the firmware accepts non-standard image container
// dimensions (we've only ever shipped 288×144 tiles before — Q5/Q7
// established that BMPs don't render unless they match the container
// size; this probe tests both directions of that constraint at a
// smaller size).
const char* g2ProbeImageQ18MixedIcon() {
  EXT_RAM_BSS_ATTR static char ret[260];
  const G2ContainerGeom listGeom = G2_GEOM_LARGE;
  // 80×80 icon in the top-right corner. 80 px @ 4 bpp = 40 bytes/row,
  // already 4-byte aligned for BMP stride; total pixel data = 3200 B,
  // header + palette = 118 B → 3318 B BMP = single Cmd=3 fragment.
  return runMixedListImageProbe("Q18", listGeom,
                                /*imgX*/ 488, /*imgY*/ 8,
                                /*imgW*/ 80, /*imgH*/ 80,
                                ret, sizeof(ret));
}

// ─────────────────────────────────────────────────────────────────────
// Q19 — solo small-dim image. CREATE-image at 96×96 (not mixed with a
// list), push a stripes BMP, hold for tap. Goal: confirm whether the
// firmware renders solo image widgets below the 288×144 we've used
// historically. If yes, the camera streamer can use 96×96 as a
// "small-but-fast" mode (~1 fps vs 0.45 fps at 288×144).
//
// Q18 already pushes 80×80 in a *mixed* list+image widget, but mixed
// CREATE uses a different protobuf path (g2BuildCreateMixedListImage)
// that may render images differently from solo CREATE-image. Q19
// isolates the solo path.
//
// Math: 96×96 4bpp BMP = 14+40+64+(96/2)*96 = 4726 B → 2 fragments.
// At 15 ms inter-frag gap with cap=3 in-flight, push wall-clock should
// be ~550 ms (order-of-magnitude; BLE pacing dominates).
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ19SmallSolo() {
  EXT_RAM_BSS_ATTR static char ret[220];

  G2Temple* arm = pickEvenAiArm("imgQ19");
  if (!arm) return "Img Q19: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic   = allocCreateMagic();
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x17;  // 231..232 (2 frags)
  const int32_t  kImgW          = 96;
  const int32_t  kImgH          = 96;

  probeBanner("Q19: solo 96×96 image",
              kCreateMagic, kPushMagicBase, kPushMagicBase + 1,
              "tests whether solo CREATE-image renders below 288×144. "
              "If the lens shows a stripes pattern, small-dim solo "
              "rendering works — green-light for fast streaming. "
              "If lens stays blank but acks come back, firmware "
              "accepts the data but won't render at this size.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q19: pre-burst SHUTDOWN failed";

  // 96×96 4bpp BMP: ~4.7 KB. Allocate 8 KB for headroom.
  const size_t kBmpCap = 8 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.q19.bmp");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    return "Img Q19: BMP heap alloc failed";
  }
  // Stripes pattern, top-down (negative height) for intuitive on-lens
  // orientation — same convention Q18 uses.
  const size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    probePostProbeShutdown(cleanupFence);
    return "Img Q19: BMP build failed";
  }
  DEBUG_G2F("[ImgProbe] Q19 BMP %u B (%dx%d 4bpp stripes)",
            (unsigned)bmpLen, (int)kImgW, (int)kImgH);

  unsigned okFrags = 0, totalFrags = 0;
  (void)sendImageBmpMultiFragment(*arm, "Q19", kCreateMagic, kPushMagicBase,
                                  bmp, bmpLen, &okFrags, &totalFrags,
                                  /*tolerateMissedAcks*/ 0,
                                  kImgW, kImgH);
  free(bmp);

  DEBUG_G2F("[ImgProbe] Q19 — image up, double-tap to dismiss (60 s cap)");
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  DEBUG_G2F("[ImgProbe] Q19 — hold ended via %s",
            tapped ? "user tap" : "60 s timeout");

  probePostProbeShutdown(cleanupFence);
  probeFooter("Q19", okFrags, totalFrags);
  snprintf(ret, sizeof(ret),
           "Img Q19: solo 96×96, %u/%u frags acked — %s — "
           "did the lens render the stripes?",
           okFrags, totalFrags, tapped ? "user tap" : "60s timeout");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q20 / Q22 / Q23 / Q26 / Q27 — shared solo live tile: stripes + moving bar,
// double-tap / 30 s safety window. Each probe uses disjoint magic bands so
// logs stay grep-clean.
//
// `framePaceMs`: minimum interval between frame starts. Pass 0 to use
// the global `gG2LiveRateMs` (CLI `g2liverate`). Smaller values push
// the lens harder when BLE + firmware keep up (see log line timings).
// ─────────────────────────────────────────────────────────────────────
static const char* probeLiveScrollingBarSolo(
    char* ret, size_t retCap,
    const char* probeId,
    const char* pickTag,
    uint32_t firstPushBase,
    int32_t imgW, int32_t imgH, int32_t barH,
    uint32_t safetyCapFrames,
    uint32_t framePaceMs,
    const char* bannerTitle,
    const char* bannerSub) {
  G2Temple* arm = pickEvenAiArm(pickTag);
  if (!arm) {
    snprintf(ret, retCap, "Img %s: no reachable temple", probeId);
    return ret;
  }
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kSafetyCapMs = 30000;
  const uint32_t bannerMagicHi = firstPushBase + 7;

  probeBanner(bannerTitle, 0, firstPushBase, bannerMagicHi, bannerSub);
  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(ret, retCap, "Img %s: pre-burst SHUTDOWN failed", probeId);
    return ret;
  }

  size_t kBmpCap = bmp4bppFileBytesU32((uint32_t)imgW, (uint32_t)imgH) + 256;
  if (kBmpCap < 4096) kBmpCap = 4096;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.liveBar");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    snprintf(ret, retCap, "Img %s: BMP heap alloc failed", probeId);
    return ret;
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;

  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  const uint32_t paceBudgetMs =
      (framePaceMs == 0) ? gG2LiveRateMs : framePaceMs;

  uint32_t pushMagicBase = firstPushBase;
  bool     createdOnce   = false;
  G2ImageCreateFence createFence = {};
  unsigned framesOk = 0;
  unsigned framesAttempted = 0;
  unsigned recreateCount = 0;
  const uint32_t startMs = millis();
  bool dismissed = false;
  bool lensTimedOut = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < safetyCapFrames) {
    if (gImgProbeHoldTapPending) {
      dismissed = true;
      break;
    }

    if (createdOnce && !arm->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] %s lens idle-timeout (recreate #%u)",
                  probeId, recreateCount);
        createdOnce = false;
      } else {
        lensTimedOut = true;
        break;
      }
    }

    size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, imgW, -imgH, BMP_PAT_STRIPES);
    if (bmpLen == 0) break;
    const int32_t denom = imgH - barH;
    const int32_t barY = (denom > 0)
        ? (int32_t)((framesAttempted * (uint32_t)barH) % (uint32_t)denom)
        : 0;
    bmpDrawRect4bpp(bmp + kPixOffset, imgW, -imgH,
                    /*x*/ 0, /*y*/ barY, /*w*/ imgW, /*h*/ barH, /*idx*/ 15);

    const unsigned fragsThisFrame =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    if (pushMagicBase + fragsThisFrame > 256) pushMagicBase = firstPushBase;

    const uint32_t frameStartMs = millis();
    bool ok = false;
    if (!createdOnce) {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpMultiFragment(*arm, probeId,
                                     allocCreateMagic(), pushMagicBase,
                                     bmp, bmpLen, &okFrags, &totalFrags,
                                     /*tolerateMissedAcks*/ 0,
                                     imgW, imgH, G2_IMG_COMPRESS_RAW,
                                     &createFence);
      createdOnce = ok;
      if (ok) {
        arm->containerReady  = true;
        arm->containerIsList = false;
        g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);
      }
    } else {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpFragmentsNoCreate(*arm, probeId,
                                         pushMagicBase,
                                         /*cid*/ 2, /*cname*/ "imgQ4",
                                         bmp, bmpLen, &okFrags, &totalFrags,
                                         /*tolerateMissedAcks*/ 0,
                                         /*inFlightCapOverride*/ 0,
                                         G2_IMG_COMPRESS_RAW, nullptr,
                                         createFence.generation,
                                         /*markExpectedEcho*/ false,
                                         createFence.lifecycleEpoch);
    }
    pushMagicBase += fragsThisFrame;
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] %s frame %u: %s in %u ms (pace=%u ms)",
              probeId, framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)paceBudgetMs);

    if (frameMs < paceBudgetMs) {
      const uint32_t sleepMs = paceBudgetMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) {
          dismissed = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;
  free(bmp);

  const char* reason = dismissed    ? "user tap"
                     : lensTimedOut ? "lens timeout"
                                    : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] %s ended (%s): %u/%u frames %u recreates in %u ms "
            "(avg %u ms/frame)",
            probeId, reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs,
            framesAttempted ? (unsigned)(totalMs / framesAttempted) : 0u);

  probePostProbeShutdown(createFence.arm ? createFence : cleanupFence);
  probeFooter(probeId, framesOk, framesAttempted);
  snprintf(ret, retCap,
           "Img %s: %u/%u frames (%dx%d) in %u ms, %u re-creates, end via %s",
           probeId, framesOk, framesAttempted, (int)imgW, (int)imgH,
           (unsigned)totalMs, recreateCount, reason);
  return ret;
}

// Q20 — live 96×96 (Animated Icons menu). Same transport as Q13 bar
// pattern; fewer Cmd=3 fragments per frame than 288×144.
const char* g2ProbeImageQ20LiveTile96() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q20", "imgQ20",
      G2_MAGIC_IMAGE_BASE + 0x25,
      96, 96, 8,
      /*safetyCapFrames*/ 45,
      /*framePaceMs*/ 0,
      "Q20: live 96x96 tile @ rate (Animated Icons)",
      "stripes + bar in 96x96 solo container; g2liverate ms between frames "
      "(default 600). Logs measured ms/frame vs requested cadence.");
}

// Q22 — live 32×32 scrolling bar (icon-sized cadence bench).
const char* g2ProbeImageQ22LiveTile32() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q22", "imgQ22",
      G2_MAGIC_IMAGE_BASE + 40,  // 250
      32, 32, 4,
      /*safetyCapFrames*/ 200,
      /*framePaceMs*/ 280,
      "Q22: live 32x32 bar @ rate (Animated Icons)",
      "same bar pattern as Q20 at 32x32; fixed ~280 ms pace (override "
      "g2liverate for this probe). Compare vs Q23/Q20.");
}

// Q23 — live 64×64 scrolling bar.
const char* g2ProbeImageQ23LiveTile64() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q23", "imgQ23",
      G2_MAGIC_IMAGE_BASE + 44,  // 254
      64, 64, 6,
      /*safetyCapFrames*/ 120,
      /*framePaceMs*/ 380,
      "Q23: live 64x64 bar @ rate (Animated Icons)",
      "same bar pattern as Q20 at 64x64; fixed ~380 ms pace (more payload "
      "than Q22). Compare ms/frame vs 32/96.");
}

// Q26 — live 124×124 scrolling bar (between 96 and max solo tile).
const char* g2ProbeImageQ26LiveTile124() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q26", "imgQ26",
      G2_MAGIC_IMAGE_BASE + 35,  // 245
      124, 124, 10,
      /*safetyCapFrames*/ 32,
      /*framePaceMs*/ 0,
      "Q26: live 124x124 bar @ rate (Animated Icons)",
      "same stripe+bar pattern as Q20; cadence g2liverate. Heavier BLE load "
      "than 96x96 — watch fragment timings.");
}

// Q27 — live 144×144 scrolling bar (max single-image container).
const char* g2ProbeImageQ27LiveTile144() {
  EXT_RAM_BSS_ATTR static char ret[260];
  return probeLiveScrollingBarSolo(
      ret, sizeof(ret),
      "Q27", "imgQ27",
      G2_MAGIC_IMAGE_BASE + 27,  // 237
      144, 144, 12,
      /*safetyCapFrames*/ 24,
      /*framePaceMs*/ 0,
      "Q27: live 144x144 bar @ rate (Animated Icons)",
      "largest solo-tile bar probe; cadence g2liverate. May need higher "
      "g2liverate ms/frame than Q20 if lens drops frames.");
}

// (Q24 procedural slime probe removed — replaced operationally by Q25
// SD-frame BMP packs, which serve the same "loop animated frames"
// purpose with arbitrary user content.)


// ─────────────────────────────────────────────────────────────────────
// Q31 / Q31b — CAN A SMALL IMAGE CONTAINER BE MOVED?
//
// Motivation: sweeping a sprite across the 576×288 canvas by repainting the
// whole screen is 24 fragments/frame. If the container itself can be
// repositioned, a 64×64 sprite costs ONE fragment (~2.1 KB) at any position —
// roughly 20× cheaper. Two candidate mechanisms, one probe each:
//
//   Q31  — Cmd=7 REBUILD carrying an ImageObject with new X/Y. Untested.
//   Q31b — SHUTDOWN+CREATE at the new X/Y. Known to work; cost unknown.
//
// They are deliberately SEPARATE rows: Q31 can wedge the EvenCore plugin task
// (docs/G2_PROTOCOL.md:1813 — one RebuildFailed and every subsequent op is
// dropped, heartbeats included, until BLE reconnects). Splitting them means a
// Q31 wedge does not block the Q31b measurement, which is the one guaranteed
// to produce a usable number.
//
// Both draw a 32×32 filled block inside a 64×64 container so the operator can
// see WHICH pixels moved: if the block marches across the lens the container
// moved; if the block only shuffles inside a stationary 64×64 window, the
// geometry was ignored and the pixels are all that changed.
//
// NOTE for both: payload is CompressMode=0 raw (this firmware has no LZ4), so
// per-frame cost is linear in container area and independent of content. A
// mostly-black frame costs exactly what a noisy one does — do not "optimise"
// these by blacking pixels out.
// ─────────────────────────────────────────────────────────────────────

// Shared geometry for the two move probes.
static constexpr int32_t kMoveTileW    = 64;
static constexpr int32_t kMoveTileH    = 64;
static constexpr int32_t kMoveBlockPx  = 32;   // sprite drawn inside the tile
static constexpr int32_t kMoveStepPx   = 64;   // container X advance per step
static constexpr int32_t kMoveSteps    = 8;    // 8 × 64 = 512 px of travel
static constexpr int32_t kMoveOriginY  = 112;  // vertically centred-ish band

// Build the probe frame: 32×32 lit block centred in an otherwise-black
// 64×64 tile. Returns BMP length, or 0 on failure.
static size_t buildMoveSpriteBmp(uint8_t* bmp, size_t cap, int32_t phase) {
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  size_t len = buildBmp4bpp(bmp, cap, kMoveTileW, -kMoveTileH, BMP_PAT_ALL_BLACK);
  if (len == 0) return 0;
  // Nudge the block within the tile each step so a stationary container is
  // still visibly "alive" — that is exactly how we tell the two outcomes
  // apart. A moving container shows the block sweeping across the lens; an
  // ignored-geometry REBUILD shows it jittering in one place.
  const int32_t inset = (phase & 1) ? (kMoveTileW - kMoveBlockPx) : 0;
  bmpDrawRect4bpp(bmp + kPixOffset, kMoveTileW, -kMoveTileH,
                  /*x*/ inset, /*y*/ (kMoveTileH - kMoveBlockPx) / 2,
                  /*w*/ kMoveBlockPx, /*h*/ kMoveBlockPx, /*idx*/ 15);
  return len;
}

// Q31b — SHUTDOWN+CREATE reposition benchmark. This path is known to work
// (it is just a fresh page); the open question is whether it is fast enough
// to animate with. Produces the ms/step number that decides it.
const char* g2ProbeImageQ31bRecreateMove() {
  EXT_RAM_BSS_ATTR static char ret[300];
  G2Temple* arm = pickEvenAiArm("imgQ31b");
  if (!arm) { snprintf(ret, sizeof(ret), "Img Q31b: no reachable temple"); return ret; }
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kPushBase    = G2_MAGIC_IMAGE_BASE + 6;   // 216

  probeBanner("Q31b: SHUTDOWN+CREATE reposition",
              0, kPushBase, kPushBase + 7,
              "re-CREATEs a 64x64 container at marching X. Block SHOULD sweep "
              "right (this path is known-good). The number that matters is "
              "ms/step in the log — that is your per-frame cost for "
              "position-animated sprites.");

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(ret, sizeof(ret), "Img Q31b: pre-probe SHUTDOWN failed");
    return ret;
  }

  const size_t kBmpCap = bmp4bppFileBytesU32(kMoveTileW, kMoveTileH) + 256;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.q31b");
  if (!bmp) {
    probePostProbeShutdown(cleanupFence);
    snprintf(ret, sizeof(ret), "Img Q31b: BMP alloc failed");
    return ret;
  }

  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  unsigned stepsOk = 0, stepsTried = 0;
  uint32_t totalMs = 0, worstMs = 0;
  uint32_t pushMagic = kPushBase;
  G2ImageCreateFence surfaceFence = cleanupFence;
  if (!imageCreateFenceCurrent(surfaceFence)) {
    free(bmp);
    gImgProbeHoldActive = false;
    probePostProbeShutdown(surfaceFence);
    snprintf(ret, sizeof(ret), "Img Q31b: initial lifecycle fence revoked");
    return ret;
  }

  for (int32_t step = 0; step < kMoveSteps && !gImgProbeHoldTapPending; step++) {
    const int32_t newX = kMoveStepPx * step;
    if (newX + kMoveTileW > 576) break;
    stepsTried++;

    const uint32_t t0 = millis();

    // Full teardown so the next CREATE lands a container at the new rect.
    // (This is the SHUTDOWN half of the cost we are measuring.)
    if (!tearDownActiveContainerAtGeneration(
            *arm, surfaceFence.generation, /*force*/ true,
            surfaceFence.lifecycleEpoch)) {
      DEBUG_G2F("[ImgProbe] Q31b step %d: fenced SHUTDOWN failed",
                (int)step);
      break;
    }

    size_t bmpLen = buildMoveSpriteBmp(bmp, kBmpCap, step);
    uint8_t cbuf[256];
    const uint32_t createMagic = allocCreateMagic();
    if (!probePrepImageCreateAck(createMagic, arm)) {
      DEBUG_G2F("[ImgProbe] Q31b step %d: CREATE lifecycle rejected",
                (int)step);
      break;
    }
    G2ImageCreateFence createFence = {};
    if (!captureImageCreateFence(*arm, &createFence)) {
      DEBUG_G2F("[ImgProbe] Q31b step %d: CREATE fence capture failed",
                (int)step);
      g2CreateTxnRelease();
      break;
    }
    size_t cn = g2BuildCreateImage(allocSeq(), createMagic, "imgQ4", 2,
                                   (uint32_t)newX, (uint32_t)kMoveOriginY,
                                   (uint32_t)kMoveTileW, (uint32_t)kMoveTileH,
                                   BLOCKS_WIDGET_ID, cbuf, sizeof(cbuf));
    if (cn == 0 || !g2SendCreateTxnEnvelope(*arm, cbuf, cn)) {
      DEBUG_G2F("[ImgProbe] Q31b step %d: CREATE send failed", (int)step);
      g2CreateTxnRelease();
      break;
    }
    if (!probeWaitImageCreateAck(createMagic, 2000, arm,
                                 createFence.generation,
                                 /*retainForPublication*/ true)) {
      DEBUG_G2F("[ImgProbe] Q31b step %d: CREATE not acked — aborting", (int)step);
      break;
    }
    uint32_t presentationEpoch = 0;
    if (!g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID,
                             createFence.lifecycleEpoch,
                             &presentationEpoch)) {
      DEBUG_G2F("[ImgProbe] Q31b step %d: CREATE publication revoked",
                (int)step);
      break;
    }
    surfaceFence = createFence;

    unsigned o = 0, t = 0;
    if (pushMagic + 2 > 256) pushMagic = kPushBase;
    const bool pushed = sendImageBmpFragmentsNoCreate(*arm, "Q31b", pushMagic,
                                                      2, "imgQ4", bmp, bmpLen, &o, &t,
                                                      /*tolerateMissedAcks*/ 0,
                                                      /*inFlightCapOverride*/ 0,
                                                      G2_IMG_COMPRESS_RAW, nullptr,
                                                      createFence.generation,
                                                      /*markExpectedEcho*/ false,
                                                      createFence.lifecycleEpoch);
    pushMagic += (t ? t : 1);

    const uint32_t dtMs = millis() - t0;
    totalMs += dtMs;
    if (dtMs > worstMs) worstMs = dtMs;
    if (pushed) stepsOk++;
    DEBUG_G2F("[ImgProbe] Q31b step %d → x=%d: %s, %u ms "
              "(incl. %u ms shutdown settle)",
              (int)step, (int)newX, pushed ? "OK" : "PUSH FAIL",
              (unsigned)dtMs, (unsigned)kImgShutdownSettleMs);

    DEBUG_G2F("[ImgProbe] Q31b step %d committed presentation epoch=%u",
              (int)step, (unsigned)presentationEpoch);
  }

  free(bmp);
  gImgProbeHoldActive = false;
  probeFooter("Q31b", stepsOk, stepsTried);
  probePostProbeShutdown(surfaceFence);

  const unsigned avg = stepsOk ? (unsigned)(totalMs / stepsOk) : 0;
  snprintf(ret, sizeof(ret),
           "Q31b: %u/%u steps, avg %u ms/step (worst %u). Settle is %u ms of "
           "that — drop it to find the floor. <300ms = usable animation.",
           stepsOk, stepsTried, avg, (unsigned)worstMs,
           (unsigned)kImgShutdownSettleMs);
  return ret;
}


// ─────────────────────────────────────────────────────────────────────
// Q32 / Q32f — LZ4 IMAGE COMPRESSION (CompressMode=2)
//
// even_hub_sdk 0.0.12 started LZ4-compressing image raw data and stamping
// CompressMode=2. Everything this firmware has ever sent is CompressMode=0
// raw BMP, so these two probes are how we find out what the glasses on the
// bench actually accept, and what it buys.
//
// Two probes because there are two separate questions and mixing them
// produces an unreadable result:
//
//   Q32f — WHICH FRAMING? The public SDK only sets the flag; the actual
//          compression happens in the phone app's closed native bridge, so
//          the byte-level framing is a genuine unknown. Q32f pushes the same
//          frame under each candidate framing and holds it on the lens so
//          the operator can see which one paints. RUN THIS FIRST.
//
//   Q32  — WHAT DOES IT BUY? Once a framing is known-good, Q32 A/Bs raw
//          against LZ4 on identical content and reports bytes, fragment
//          count and ms/frame for each.
//
// Q32 deliberately measures TWO content types. The playbook's central claim
// is that compressibility — not pixel count — sets the cost, so a probe that
// only ever pushed a friendly frame would report a flattering number that
// says nothing about a photo-like frame. ART is block-quantized bars on
// black (what real UI looks like); NOISE is incompressible by construction
// and is the honest worst case.
//
// AN ACK IS NOT A PAINT. Both probes report ack counts because that is all
// the firmware tells us; only the wearer can confirm pixels. If Q32f says
// every framing acked, believe the lens, not the count.
// ─────────────────────────────────────────────────────────────────────

// Candidate byte-level framings for a CompressMode=2 payload. Bare block is
// what LZ4_compress_default emits; SIZE_PREFIX is the common embedded
// convention (block format is not self-describing, so pairings often prepend
// the decompressed length); the two FRAME variants are the self-describing
// container, which is what a host using a high-level LZ4 binding would most
// likely produce.
typedef enum {
  G2_LZ4_WRAP_BLOCK = 0,
  G2_LZ4_WRAP_SIZE_PREFIX,
  G2_LZ4_WRAP_FRAME,
  G2_LZ4_WRAP_FRAME_CSIZE,
} G2Lz4Wrap;

static const char* g2Lz4WrapName(G2Lz4Wrap w) {
  switch (w) {
    case G2_LZ4_WRAP_BLOCK:       return "block (bare)";
    case G2_LZ4_WRAP_SIZE_PREFIX: return "block + u32 size prefix";
    case G2_LZ4_WRAP_FRAME:       return "frame (no content size)";
    case G2_LZ4_WRAP_FRAME_CSIZE: return "frame + content size";
  }
  return "?";
}

// Encode `src` into `dst` under one framing. Returns payload length, or 0.
static size_t g2Lz4WrapPayload(G2Lz4Wrap wrap,
                               const uint8_t* src, size_t srcLen,
                               uint8_t* dst, size_t dstCap) {
  switch (wrap) {
    case G2_LZ4_WRAP_BLOCK:
      return lz4CompressBlock(src, srcLen, dst, dstCap);

    case G2_LZ4_WRAP_SIZE_PREFIX: {
      if (dstCap < 4) return 0;
      dst[0] = (uint8_t)( srcLen        & 0xFF);
      dst[1] = (uint8_t)((srcLen >>  8) & 0xFF);
      dst[2] = (uint8_t)((srcLen >> 16) & 0xFF);
      dst[3] = (uint8_t)((srcLen >> 24) & 0xFF);
      const size_t n = lz4CompressBlock(src, srcLen, dst + 4, dstCap - 4);
      return n ? n + 4 : 0;
    }

    case G2_LZ4_WRAP_FRAME:
      return lz4CompressFrame(src, srcLen, dst, dstCap, /*contentSize*/ false);

    case G2_LZ4_WRAP_FRAME_CSIZE:
      return lz4CompressFrame(src, srcLen, dst, dstCap, /*contentSize*/ true);
  }
  return 0;
}

// Geometry shared by both probes: the largest legal single container, which
// is where the fragment-count difference is most visible (6 raw fragments vs
// 1 compressed for typical art).
static constexpr int32_t kLz4TileW = 288;
static constexpr int32_t kLz4TileH = 144;

// Content builders. Both write a complete 4-bpp BMP into `bmp`.

// ART — block-quantized horizontal bars on black. Deliberately built the way
// the playbook says to render for the compressor: long identical byte runs,
// a small brightness alphabet, most of the frame black.
static size_t buildLz4ArtBmp(uint8_t* bmp, size_t cap, uint32_t phase) {
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  size_t len = buildBmp4bpp(bmp, cap, kLz4TileW, -kLz4TileH, BMP_PAT_ALL_BLACK);
  if (len == 0) return 0;
  // Four bars at two brightness levels, marching down by phase so successive
  // frames are not byte-identical (which would let any duplicate-frame
  // suppression in the firmware flatter the measurement).
  for (int i = 0; i < 4; i++) {
    const int32_t y = (int32_t)(((phase * 8u) + (uint32_t)i * 32u) % 128u);
    bmpDrawRect4bpp(bmp + kPixOffset, kLz4TileW, -kLz4TileH,
                    /*x*/ 24 + i * 16, /*y*/ y,
                    /*w*/ kLz4TileW - 48 - i * 32, /*h*/ 12,
                    /*idx*/ (i & 1) ? 15 : 8);
  }
  return len;
}

// NOISE — every nibble pseudo-random. Worst case for LZ4 by construction:
// there is nothing to match, so the encoder can only emit literals and the
// payload comes back very slightly LARGER than the input.
static size_t buildLz4NoiseBmp(uint8_t* bmp, size_t cap, uint32_t phase) {
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  size_t len = buildBmp4bpp(bmp, cap, kLz4TileW, -kLz4TileH, BMP_PAT_ALL_BLACK);
  if (len == 0) return 0;
  uint32_t s = 0x9E3779B9u ^ (phase * 2654435761u);
  for (size_t i = kPixOffset; i < len; i++) {
    s = s * 1103515245u + 12345u;
    bmp[i] = (uint8_t)(s >> 16);
  }
  return len;
}

typedef size_t (*Lz4FrameBuilder)(uint8_t* bmp, size_t cap, uint32_t phase);

struct Lz4LegResult {
  size_t   bytes;     // mean payload bytes actually put on the wire
  unsigned frags;     // image-layer fragments in the last rep
  uint32_t meanMs;    // mean send-to-all-acked time
  uint32_t encMs;     // mean host-side compression time (0 on the raw leg)
  bool     allAcked;
};

// One timed leg of the A/B: `reps` frame pushes into an already-CREATEd
// container, reported as means.
//
// The frame is REBUILT with a fresh phase every rep rather than pushing one
// buffer repeatedly. Identical consecutive frames are exactly what a
// duplicate-frame optimisation anywhere in the path would skip, and a
// skipped frame reads as a very fast frame — which would quietly turn this
// benchmark into a measurement of silence.
//
// Compression time is tracked separately from send time. It is host-side
// work, not link time, and at these sizes it is small; folding it into
// ms/frame would misattribute it either way.
static void lz4TimeLeg(G2Temple& arm, const char* tag,
                       uint32_t pushMagicBase, bool useLz4, G2Lz4Wrap wrap,
                       Lz4FrameBuilder build,
                       uint8_t* bmp, size_t bmpCap,
                       uint8_t* comp, size_t compCap,
                       unsigned reps, Lz4LegResult* out,
                       const G2ImageCreateFence& createFence) {
  uint64_t totalMs = 0, totalEnc = 0, totalBytes = 0;
  unsigned okReps = 0, done = 0;

  for (unsigned r = 0; r < reps && !gImgProbeAbort; r++) {
    const size_t bmpLen = build(bmp, bmpCap, r + 1);
    if (bmpLen == 0) break;

    const uint8_t* payload = bmp;
    size_t   payloadLen    = bmpLen;
    uint32_t encMs         = 0;
    if (useLz4) {
      const uint32_t encStart = millis();
      payloadLen = g2Lz4WrapPayload(wrap, bmp, bmpLen, comp, compCap);
      encMs   = millis() - encStart;
      payload = comp;
      if (payloadLen == 0) {
        DEBUG_G2F("[ImgProbe] %s rep %u: LZ4 encode failed — leg truncated",
                  tag, r + 1);
        break;
      }
    }

    const unsigned frags =
        (unsigned)((payloadLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) /
                   G2_IMG_MAPRAW_CHUNK_BYTES);

    // The magic band is reused every rep: each push arms and drains its own
    // ack window before returning, so reps never overlap.
    unsigned okFrags = 0, totalFrags = 0;
    const uint32_t t0 = millis();
    const bool ok = sendImageBmpFragmentsNoCreate(
        arm, tag, pushMagicBase, /*cid*/ 2, /*cname*/ "imgQ4",
        payload, payloadLen, &okFrags, &totalFrags,
        /*tolerateMissedAcks*/ 0, /*inFlightCapOverride*/ 0,
        useLz4 ? G2_IMG_COMPRESS_LZ4 : G2_IMG_COMPRESS_RAW,
        nullptr, createFence.generation,
        /*markExpectedEcho*/ false, createFence.lifecycleEpoch);
    const uint32_t dtMs = millis() - t0;

    totalMs    += dtMs;
    totalEnc   += encMs;
    totalBytes += payloadLen;
    done++;
    if (ok) okReps++;
    if (out) out->frags = frags;

    DEBUG_G2F("[ImgProbe] %s rep %u/%u: %s in %u ms (%u B, %u frag, enc %u ms)",
              tag, r + 1, reps, ok ? "OK" : "FAIL", (unsigned)dtMs,
              (unsigned)payloadLen, frags, (unsigned)encMs);
  }

  if (!out) return;
  out->bytes    = done ? (size_t)(totalBytes / done)  : 0;
  out->meanMs   = done ? (uint32_t)(totalMs / done)   : 0;
  out->encMs    = done ? (uint32_t)(totalEnc / done)  : 0;
  out->allAcked = (done == reps) && (okReps == reps);
}

// Q32 hard-wires production framing (bare block; Q16d / Even App).
// Framing sweep (Q32f) retired from the Image test menus.
static G2Lz4Wrap gQ32Wrap = G2_LZ4_WRAP_BLOCK;

// Q32 — the A/B. Answers "what does LZ4 actually buy on this link?"
const char* g2ProbeImageQ32Lz4Benchmark() {
  EXT_RAM_BSS_ATTR static char ret[320];
  G2Temple* arm = pickEvenAiArm("imgQ32");
  if (!arm) { snprintf(ret, sizeof(ret), "Img Q32: no reachable temple"); return ret; }
  G2ImageCreateFence cleanupFence = {};

  // Keep raw legs truly raw — auto LZ4 would collapse the A/B into LZ4/LZ4.
  G2ImgLz4AutoOffGuard lz4AutoOff;

  // CREATE uses a fresh low-byte token. Keep the six-fragment Cmd=3 band
  // inside the firmware's uint8 limit; raw-vs-LZ4 legs intentionally reuse it
  // after each ACK window drains.
  const uint32_t kCreateMagic = allocCreateMagic();
  const uint32_t kPushBase    = G2_MAGIC_IMAGE_BASE + 0x27;  // 249..254 (6 frags)
  const unsigned kReps        = 5;   // playbook: short runs jitter ±10 ms

  probeBanner("Q32: LZ4 A/B bare-block (CompressMode 0 vs 2)",
              kCreateMagic, kPushBase, kPushBase + 5,
              "288x144, two content types x two modes, 5 reps each. ART is "
              "block-quantized bars (compressible); NOISE is random nibbles "
              "(incompressible). WATCH THE LENS: bars marching = LZ4 painted. "
              "Treat <15 ms deltas as noise.");
  DEBUG_G2F("[ImgProbe] Q32 framing: '%s' (production)",
            g2Lz4WrapName(gQ32Wrap));

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(ret, sizeof(ret), "Img Q32: pre-probe SHUTDOWN failed");
    return ret;
  }

  const size_t kBmpCap  = bmp4bppFileBytesU32(kLz4TileW, kLz4TileH) + 256;
  const size_t kCompCap = lz4FrameBound(kBmpCap);
  uint8_t* bmp  = (uint8_t*)ps_alloc(kBmpCap,  AllocPref::PreferPsram, "g2.img.q32.bmp");
  uint8_t* comp = (uint8_t*)ps_alloc(kCompCap, AllocPref::PreferPsram, "g2.img.q32.lz4");
  if (!bmp || !comp) {
    free(bmp); free(comp);
    probePostProbeShutdown(cleanupFence);
    snprintf(ret, sizeof(ret), "Img Q32: alloc failed");
    return ret;
  }

  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  // CREATE once, outside the timed legs — we are measuring frame updates,
  // not container setup.
  const size_t bmpLen = buildLz4ArtBmp(bmp, kBmpCap, 0);
  unsigned okFrags = 0, totalFrags = 0;
  G2ImageCreateFence createFence = {};
  const bool created = sendImageBmpMultiFragment(
      *arm, "Q32/create", kCreateMagic, kPushBase, bmp, bmpLen,
      &okFrags, &totalFrags, /*tolerateMissedAcks*/ 0,
      kLz4TileW, kLz4TileH, G2_IMG_COMPRESS_RAW, &createFence);
  if (!created) {
    gImgProbeHoldActive = false;
    free(bmp); free(comp);
    probePostProbeShutdown(createFence.arm ? createFence : cleanupFence);
    snprintf(ret, sizeof(ret), "Img Q32: CREATE+first push failed (%u/%u frags)",
             okFrags, totalFrags);
    return ret;
  }
  arm->containerReady  = true;
  arm->containerIsList = false;
  g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);

  Lz4LegResult art[2]   = {};   // [0] = raw, [1] = lz4
  Lz4LegResult noise[2] = {};

  for (int content = 0; content < 2 && !gImgProbeAbort; content++) {
    const bool isArt      = (content == 0);
    Lz4LegResult* legs    = isArt ? art : noise;
    Lz4FrameBuilder build = isArt ? buildLz4ArtBmp : buildLz4NoiseBmp;
    const char* cName     = isArt ? "ART" : "NOISE";

    // Leg A — raw, today's path. This is the number to beat.
    lz4TimeLeg(*arm, isArt ? "Q32/art-raw" : "Q32/noise-raw", kPushBase,
               /*useLz4*/ false, gQ32Wrap, build, bmp, kBmpCap, comp, kCompCap,
               kReps, &legs[0], createFence);

    // Leg B — LZ4, same content, same reps, same magic band.
    lz4TimeLeg(*arm, isArt ? "Q32/art-lz4" : "Q32/noise-lz4", kPushBase,
               /*useLz4*/ true, gQ32Wrap, build, bmp, kBmpCap, comp, kCompCap,
               kReps, &legs[1], createFence);

    DEBUG_G2F("[ImgProbe] Q32 %s: raw %u B/%u frag/%u ms%s  vs  "
              "lz4 %u B/%u frag/%u ms (+%u ms encode)%s",
              cName,
              (unsigned)legs[0].bytes, legs[0].frags, (unsigned)legs[0].meanMs,
              legs[0].allAcked ? "" : " (ACKS MISSING)",
              (unsigned)legs[1].bytes, legs[1].frags, (unsigned)legs[1].meanMs,
              (unsigned)legs[1].encMs,
              legs[1].allAcked ? "" : " (ACKS MISSING)");
  }

  gImgProbeHoldActive = false;
  free(bmp);
  free(comp);
  probePostProbeShutdown(createFence);
  probeFooter("Q32",
              (unsigned)(art[0].allAcked + art[1].allAcked +
                         noise[0].allAcked + noise[1].allAcked),
              4u);

  // Ratios in tenths — this firmware builds without float formatting in
  // DEBUG/snprintf paths, and integer tenths read fine at a glance.
  const unsigned artSizeX10 =
      art[1].bytes ? (unsigned)((art[0].bytes * 10) / art[1].bytes) : 0;
  const unsigned artTimeX10 =
      art[1].meanMs ? (unsigned)((art[0].meanMs * 10) / art[1].meanMs) : 0;
  const unsigned noiseSizeX10 =
      noise[1].bytes ? (unsigned)((noise[0].bytes * 10) / noise[1].bytes) : 0;

  snprintf(ret, sizeof(ret),
           "Q32 art: %uB/%uf/%ums -> %uB/%uf/%ums (%u.%ux smaller, %u.%ux "
           "faster, +%ums enc)%s | noise: %uB/%ums -> %uB/%ums (%u.%ux) | "
           "LZ4 acks %s",
           (unsigned)art[0].bytes, art[0].frags, (unsigned)art[0].meanMs,
           (unsigned)art[1].bytes, art[1].frags, (unsigned)art[1].meanMs,
           artSizeX10 / 10, artSizeX10 % 10,
           artTimeX10 / 10, artTimeX10 % 10,
           (unsigned)art[1].encMs,
           art[1].allAcked ? "" : " [!]",
           (unsigned)noise[0].bytes, (unsigned)noise[0].meanMs,
           (unsigned)noise[1].bytes, (unsigned)noise[1].meanMs,
           noiseSizeX10 / 10, noiseSizeX10 % 10,
           (art[1].allAcked && noise[1].allAcked) ? "OK" : "INCOMPLETE");
  return ret;
}


// Q25: directory path for frame_XX.bmp sequence (set before worker spawn).
EXT_RAM_BSS_ATTR static char s_q25PackDir[128];

void g2ProbeImageQ25SetPackPath(const char* dirPath) {
  s_q25PackDir[0] = '\0';
  if (!dirPath || !dirPath[0]) return;
  // Packs must live under G2_ICON_ANIMATIONS_VFS_PATH (no ".." traversal).
  if (strstr(dirPath, "..")) {
    DEBUG_G2F("[G2] Q25 pack path rejected (path traversal)");
    return;
  }
  const char* const root = G2_ICON_ANIMATIONS_VFS_PATH;
  const size_t rootLen = strlen(root);
  if (strncmp(dirPath, root, rootLen) != 0 || dirPath[rootLen] != '/') {
    DEBUG_G2F("[G2] Q25 pack path rejected (must be subdirectory of %s/)", root);
    return;
  }
  strlcpy(s_q25PackDir, dirPath, sizeof(s_q25PackDir));
  size_t n = strlen(s_q25PackDir);
  while (n > 0 && (s_q25PackDir[n - 1] == '/' || s_q25PackDir[n - 1] == '\\')) {
    s_q25PackDir[--n] = '\0';
  }
}

// ─────────────────────────────────────────────────────────────────────
// Q25 — loop 4bpp BMPs from a VFS directory (frame_00.bmp …). Path is set
// via g2ProbeImageQ25SetPackPath before the test worker runs. Only subdirs of
// G2_ICON_ANIMATIONS_VFS_PATH subdirs are accepted (see gif_to_g2_bmps.py).
// All frames are read from SD/VFS once into PSRAM (g2ReadBmp4bppFromVfs); the
// animation loop only reuses those buffers — no per-frame card reads.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ25SdFrameAnimation() {
  EXT_RAM_BSS_ATTR static char ret[320];
  EXT_RAM_BSS_ATTR static char pathBuf[192];

  G2Temple* arm = pickEvenAiArm("imgQ25");
  if (!arm) {
    snprintf(ret, sizeof(ret), "Img Q25: no reachable temple");
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }
  G2ImageCreateFence cleanupFence = {};
  if (!s_q25PackDir[0]) {
    snprintf(ret, sizeof(ret), "Img Q25: no pack directory set");
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }

  // Cap on consecutive frame_XX.bmp files scanned; PSRAM holds only frameCount
  // loaded buffers (see loop below), not kQ25PackMaxFrames worth of BMPs.
  static constexpr unsigned kQ25PackMaxFrames = 64;
  unsigned frameCount = 0;
  for (unsigned i = 0; i < kQ25PackMaxFrames; i++) {
    snprintf(pathBuf, sizeof(pathBuf), "%s/frame_%02u.bmp", s_q25PackDir, i);
    if (!VFS::existsGuarded(String(pathBuf), currentAuthContext())) break;
    frameCount++;
  }
  if (frameCount == 0) {
    snprintf(ret, sizeof(ret), "Img Q25: no frame_00.bmp in %s", s_q25PackDir);
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }

  uint8_t* frameCache[kQ25PackMaxFrames] = {};
  size_t   frameLen[kQ25PackMaxFrames]   = {};
  int32_t  imgW = 0;
  int32_t  imgH = 0;
  const char* loadErr = "";

  for (unsigned i = 0; i < frameCount; i++) {
    snprintf(pathBuf, sizeof(pathBuf), "%s/frame_%02u.bmp", s_q25PackDir, i);
    uint8_t* bmp = nullptr;
    size_t   blen = 0;
    int32_t  fw = 0, fh = 0;
    loadErr = "";
    if (!g2ReadBmp4bppFromVfs(pathBuf, &bmp, &blen, &fw, &fh, &loadErr)) {
      for (unsigned j = 0; j < i; j++) {
        if (frameCache[j]) {
          free(frameCache[j]);
          frameCache[j] = nullptr;
        }
      }
      snprintf(ret, sizeof(ret), "Img Q25: frame_%02u: %s", i,
               loadErr ? loadErr : "?");
      g2ProbeImageQ25SetPackPath(nullptr);
      return ret;
    }
    if (i == 0) {
      imgW = fw;
      imgH = fh;
    } else if (fw != imgW || fh != imgH) {
      free(bmp);
      for (unsigned j = 0; j < i; j++) {
        if (frameCache[j]) {
          free(frameCache[j]);
          frameCache[j] = nullptr;
        }
      }
      snprintf(ret, sizeof(ret), "Img Q25: frame_%02u size mismatch", i);
      g2ProbeImageQ25SetPackPath(nullptr);
      return ret;
    }
    frameCache[i] = bmp;
    frameLen[i]   = blen;
    // Yield between frame reads so the IDLE task on this core can reset
    // the task watchdog. Without it, loading a multi-frame pack
    // (especially large ones) trips `task_wdt: IDLE1 (CPU 1) ...
    // g2_img_probe` because the SD reads stay on-CPU long enough to
    // starve the idle task. 1 tick (≈1 ms) is enough — load time
    // overhead is negligible compared to BLE push time downstream.
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  const int32_t cw = (imgW < 0) ? -imgW : imgW;
  const int32_t ch = (imgH < 0) ? -imgH : imgH;

  const uint32_t kFirstPushBase = G2_MAGIC_IMAGE_BASE + 11;  // 221
  const uint32_t kSafetyCapMs     = 30000;
  const uint32_t kSafetyCapFrames = 500;
  // Q25 cadence is user-tunable via gSettings.g2PackRateMs (CLI: g2packrate).
  // Independent of g2liverate so animation playback speed is decoupled from
  // the live-tile test cadence. Floor 20 ms keeps the worker from busy-looping;
  // upper bound 2000 ms is the setting range. For large frames the BLE push
  // dominates and this value is effectively ignored.
  const uint32_t kPaceMs          = (uint32_t)gSettings.g2PackRateMs;

  probeBanner("Q25: PSRAM-cached BMP loop", 0,
              kFirstPushBase, kFirstPushBase + 10,
              s_q25PackDir);
  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    for (unsigned i = 0; i < frameCount; i++) {
      if (frameCache[i]) {
        free(frameCache[i]);
        frameCache[i] = nullptr;
      }
    }
    snprintf(ret, sizeof(ret), "Img Q25: pre-burst SHUTDOWN failed");
    g2ProbeImageQ25SetPackPath(nullptr);
    return ret;
  }

  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  const uint32_t paceBudgetMs  = kPaceMs;
  uint32_t       pushMagicBase = kFirstPushBase;
  bool           createdOnce   = false;
  G2ImageCreateFence createFence = {};
  unsigned       framesOk      = 0;
  unsigned       framesAttempted = 0;
  unsigned       recreateCount = 0;
  const uint32_t startMs       = millis();
  bool           dismissed     = false;
  bool           lensTimedOut  = false;

  while ((millis() - startMs) < kSafetyCapMs &&
         framesAttempted < kSafetyCapFrames) {
    if (gImgProbeHoldTapPending) {
      dismissed = true;
      break;
    }
    if (createdOnce && !arm->containerReady) {
      if (gG2LiveLoopKeepAlive) {
        recreateCount++;
        DEBUG_G2F("[ImgProbe] Q25 lens idle-timeout (recreate #%u)",
                  recreateCount);
        createdOnce = false;
      } else {
        lensTimedOut = true;
        break;
      }
    }

    const unsigned fi = (unsigned)(framesAttempted % frameCount);
    uint8_t* const bmp    = frameCache[fi];
    const size_t   bmpLen = frameLen[fi];

    const unsigned fragsThisFrame =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) /
                   G2_IMG_MAPRAW_CHUNK_BYTES);
    if (pushMagicBase + fragsThisFrame > 256) pushMagicBase = kFirstPushBase;

    const uint32_t frameStartMs = millis();
    bool           ok           = false;
    if (!createdOnce) {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpMultiFragment(*arm, "Q25",
                                      allocCreateMagic(), pushMagicBase,
                                      bmp, bmpLen, &okFrags, &totalFrags,
                                      /*tolerateMissedAcks*/ 0, cw, ch,
                                      G2_IMG_COMPRESS_RAW, &createFence);
      createdOnce = ok;
      if (ok) {
        arm->containerReady  = true;
        arm->containerIsList = false;
        g2LensSetContainer(true, false, BLOCKS_WIDGET_ID);
      }
    } else {
      unsigned okFrags = 0, totalFrags = 0;
      ok = sendImageBmpFragmentsNoCreate(*arm, "Q25", pushMagicBase,
                                          /*cid*/ 2, /*cname*/ "imgAnim",
                                          bmp, bmpLen, &okFrags, &totalFrags,
                                          /*tolerateMissedAcks*/ 0,
                                          /*inFlightCapOverride*/ 0,
                                          G2_IMG_COMPRESS_RAW, nullptr,
                                          createFence.generation,
                                          /*markExpectedEcho*/ false,
                                          createFence.lifecycleEpoch);
    }
    pushMagicBase += fragsThisFrame;
    framesAttempted++;
    if (ok) framesOk++;

    const uint32_t frameMs = millis() - frameStartMs;
    DEBUG_G2F("[ImgProbe] Q25 frame %u: %s in %u ms (pace=%u ms)",
              framesAttempted, ok ? "OK" : "FAIL", (unsigned)frameMs,
              (unsigned)paceBudgetMs);

    if (frameMs < paceBudgetMs) {
      const uint32_t sleepMs = paceBudgetMs - frameMs;
      const uint32_t sleepStart = millis();
      while ((millis() - sleepStart) < sleepMs) {
        if (gImgProbeHoldTapPending) {
          dismissed = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      if (dismissed) break;
    }
  }
  gImgProbeHoldActive = false;

  const char* reason = dismissed    ? "user tap"
                     : lensTimedOut ? "lens timeout"
                                    : "safety cap";
  const uint32_t totalMs = millis() - startMs;
  DEBUG_G2F("[ImgProbe] Q25 ended (%s): %u/%u frames %u recreates in %u ms",
            reason, framesOk, framesAttempted, recreateCount, (unsigned)totalMs);

  probePostProbeShutdown(createFence.arm ? createFence : cleanupFence);
  probeFooter("Q25", framesOk, framesAttempted);

  for (unsigned i = 0; i < frameCount; i++) {
    if (frameCache[i]) {
      free(frameCache[i]);
      frameCache[i] = nullptr;
    }
  }

  snprintf(ret, sizeof(ret),
           "Img Q25: %u/%u frames (%u cached PSRAM, %dx%d) in %u ms, %u re-creates, "
           "end via %s",
           framesOk, framesAttempted, frameCount, (int)cw, (int)ch,
           (unsigned)totalMs, recreateCount, reason);
  g2ProbeImageQ25SetPackPath(nullptr);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q28 — Mixed Image+Text compound with INDEPENDENT refresh rates.
//
// Hypothesis: a CREATE carrying both a TextObject and an ImageObject
// (one container, two children) accepts BOTH:
//   - Cmd=3 ImageMapRaw fragments addressed to the image child by name
//     (the "camera-stream" pattern, applied here to procedural BMPs)
//   - Cmd=7 single-child REBUILD-text addressed to the text child by name
// without either disturbing the other. docs/G2_PROTOCOL.md verifies that
// single-child REBUILD-text on a list+text compound preserves the list
// (Q-series probe g2ProbeRebuildTextChild). image+text is structurally
// the same shape (different-widget-types case), so the preservation should
// extend. Same-type compounds (text+text) have a known-broken multi-child
// blanking bug — different case, doesn't apply here.
//
// Pace: alternating BMP A/B every 750 ms (fast-cycling 96×96 patterns,
// visibly different so dropped frames are obvious), text rebuild every
// other frame (~1.5 s) showing a frame counter + elapsed-ms. 24-frame
// cap (~18 s), double-tap exits early.
//
// If this passes: the camera-stream worker can be refactored to optionally
// CREATE a compound and emit per-frame text alongside live frames (e.g.
// fps/dims/sensor readings). If it fails: existing solo-image stream
// stays as-is, no production code touched — that's the whole point of
// keeping this as a separate probe.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ28MixedImageTextLive() {
  EXT_RAM_BSS_ATTR static char ret[300];
  G2Temple* arm = pickEvenAiArm("imgQ28");
  if (!arm) {
    snprintf(ret, sizeof(ret), "Img Q28: no reachable temple");
    return ret;
  }
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic              = allocCreateMagic();
  static constexpr uint32_t kPushSlotStart = G2_MAGIC_IMAGE_BASE + 0x11;  // 227
  static constexpr uint32_t kMaxPushMagic  = 254;                          // uint8 ceiling
  static constexpr int32_t  kImgW   = 96;
  static constexpr int32_t  kImgH   = 96;
  static constexpr unsigned kFrames = 24;
  static constexpr uint32_t kPaceMs = 750;     // per-frame target
  static const char* const kImgChild = "imgQ28";
  static const char* const kTxtChild = "txtQ28";

  // Magic cycling — firmware constrains magic to uint8 (≤255). We have 28
  // push magics available (227..254), and each frame's
  // burst occupies fragsPerFrame consecutive magics. We cycle through
  // floor(28 / fragsPerFrame) slots so consecutive frames don't reuse
  // the same magic range. 96×96 4-bpp BMP is 2 fragments → 14 slots.
  // Without this cycling, the 15th frame wraps past 255 and the firmware
  // reassembler stops acking. Same pattern as the camera stream worker.
  static constexpr unsigned kFragsPerFrame = 2;  // 96×96 4-bpp = ~4.7 KB
  static constexpr unsigned kPushAvailable =
      (unsigned)(kMaxPushMagic - kPushSlotStart + 1);
  static constexpr unsigned kNumSlots = kPushAvailable / kFragsPerFrame;

  probeBanner("Q28: mixed image+text live (independent refresh)",
              kCreateMagic, kPushSlotStart, kMaxPushMagic,
              "compound CREATE: text top half + image 96x96 centered "
              "in bottom half. Image push every ~750ms, text rebuild "
              "every other frame. Double-tap to exit.");

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(ret, sizeof(ret), "Img Q28: pre-burst SHUTDOWN failed");
    return ret;
  }

  // Geometry: text top (8,0,560,130 — same as Q16's list slot), image
  // 96x96 centered horizontally in the bottom half (240,168). No overlap.
  G2TextChildSpec textChild = {};
  textChild.containerId   = 1;
  textChild.containerName = kTxtChild;
  textChild.eventCapture  = false;
  textChild.content       = "Q28 starting...";
  textChild.geom          = { 8, 0, 560, 130 };

  G2ImageTile imgTile = {};
  imgTile.x = 240; imgTile.y = 168;
  imgTile.w = (uint32_t)kImgW;
  imgTile.h = (uint32_t)kImgH;
  imgTile.containerId   = 2;
  imgTile.containerName = kImgChild;

  uint8_t createBuf[1024];
  size_t createLen = g2BuildCreateMixedImageText(
      allocSeq(), kCreateMagic,
      textChild, imgTile,
      BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(ret, sizeof(ret), "Img Q28: CREATE-mixed build failed");
    return ret;
  }

  if (!probePrepImageCreateAck(kCreateMagic, arm)) {
    snprintf(ret, sizeof(ret), "Img Q28: CREATE rejected by lifecycle fence");
    return ret;
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    snprintf(ret, sizeof(ret), "Img Q28: CREATE fence capture failed");
    return ret;
  }
  if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
    g2CreateTxnRelease();
    snprintf(ret, sizeof(ret), "Img Q28: CREATE-mixed TX failed");
    return ret;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs,
                               arm, createFence.generation)) {
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret),
             "Img Q28: CREATE-mixed ack timeout — firmware may not accept "
             "image+text composition (image+list verified, image+text new)");
    return ret;
  }
  DEBUG_G2F("[ImgProbe] Q28 CREATE-mixed acked (text@'%s' + image@'%s' %dx%d)",
            kTxtChild, kImgChild, (int)kImgW, (int)kImgH);

  // Two procedural BMPs (stripes vs all-black) — alternation makes
  // dropped frames eye-visible. 96x96 4-bpp BMP is ~4.7 KB → 2 fragments
  // per push at G2_IMG_MAPRAW_CHUNK_BYTES.
  const size_t kBmpCap = 8 * 1024;
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.q28.bmpA");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.q28.bmpB");
  if (!bmpA || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "Img Q28: BMP heap alloc failed");
    return ret;
  }
  size_t bmpALen = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  size_t bmpBLen = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_ALL_BLACK);
  if (bmpALen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpB);
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "Img Q28: BMP build failed");
    return ret;
  }

  unsigned framesPushed = 0;
  unsigned framesFailed = 0;
  unsigned textRebuilds = 0;
  unsigned textFails    = 0;
  const uint32_t startMs = millis();

  for (unsigned i = 0; i < kFrames && !gImgProbeAbort; i++) {
    const uint32_t frameStartMs = millis();
    const uint8_t* bmp    = (i & 1) ? bmpB    : bmpA;
    const size_t   bmpLen = (i & 1) ? bmpBLen : bmpALen;
    // Cycle through kNumSlots magic-bands so we never overflow uint8.
    // Slot N occupies [kPushSlotStart + N*kFragsPerFrame .. + 1]; with
    // kFragsPerFrame=2 and 14 slots that's 227..253 across 14 frames,
    // then wraps. Same pattern the camera stream worker uses.
    const uint32_t pushMagic =
        kPushSlotStart + ((uint32_t)(i % kNumSlots)) * (uint32_t)kFragsPerFrame;

    // tolerateMissedAcks=3 — streaming is forgiving; if the firmware
    // drops one ack we don't want to abort the whole 24-frame test.
    // Same value the camera stream worker uses. Auto LZ4 bare-block OK
    // on mixed children (Q16d).
    unsigned okFrags = 0, totalFrags = 0;
    bool pushed = sendImageBmpFragmentsNoCreate(
        *arm, "Q28", pushMagic,
        /*cid*/ imgTile.containerId, /*cname*/ kImgChild,
        bmp, bmpLen, &okFrags, &totalFrags,
        /*tolerateMissedAcks*/ 3, /*inFlightCapOverride*/ 0,
        G2_IMG_COMPRESS_RAW, nullptr, createFence.generation,
        /*markExpectedEcho*/ false, createFence.lifecycleEpoch);
    if (pushed) {
      framesPushed++;
    } else {
      framesFailed++;
      DEBUG_G2F("[ImgProbe] Q28 frame %u image push FAILED (%u/%u frags)",
                i, okFrags, totalFrags);
    }

    // Text rebuild every OTHER frame — different cadence from image
    // refresh is the whole point. If single-child REBUILD-text disturbs
    // the image child, we'll see image flicker on text rebuild ticks.
    if ((i & 1) == 0) {
      char txt[64];
      const uint32_t elapsed = millis() - startMs;
      snprintf(txt, sizeof(txt),
               "Q28 frame %u/%u\nimg pushed: %u\nelapsed: %u ms",
               i + 1, kFrames, framesPushed, (unsigned)elapsed);
      const G2ContainerGeom txtGeom = { 8, 0, 560, 130 };
      if (sendRebuildTextNamedAndWait(*arm, kTxtChild, txt, txtGeom,
                                      /*eventCapture*/ false)) {
        textRebuilds++;
      } else {
        textFails++;
        DEBUG_G2F("[ImgProbe] Q28 frame %u text REBUILD FAILED", i);
      }
    }

    // Pace per frame — sleep what's left of the budget in 50 ms slices,
    // checking gImgProbeAbort between each slice. Without this, a
    // double-tap during the pacing wait would take up to kPaceMs to
    // notice; with 50 ms slices the worst-case exit latency is bounded
    // to ~50 ms even when nothing else is happening. Same shape the live
    // page workers use for their inter-tick wait.
    const uint32_t consumedMs = millis() - frameStartMs;
    if (consumedMs < kPaceMs) {
      const uint32_t remainingMs = kPaceMs - consumedMs;
      const uint32_t kSliceMs = 50;
      const uint32_t sliceEndMs = deadlineAfter(millis(), remainingMs);
      while (deadlinePending(sliceEndMs, millis()) && !gImgProbeAbort) {
        const uint32_t leftMs = sliceEndMs - millis();
        const uint32_t thisMs = leftMs < kSliceMs ? leftMs : kSliceMs;
        vTaskDelay(pdMS_TO_TICKS(thisMs));
      }
    }
  }

  const bool aborted = gImgProbeAbort;
  free(bmpA); free(bmpB);
  probePostProbeShutdown(createFence);
  probeFooter("Q28", framesPushed, kFrames);

  const uint32_t totalMs = millis() - startMs;
  snprintf(ret, sizeof(ret),
           "Img Q28: %u/%u img frames (%u fail), %u/%u text rebuilds (%u fail) "
           "in %u ms (end: %s)",
           framesPushed, kFrames, framesFailed,
           textRebuilds, kFrames / 2, textFails,
           (unsigned)totalMs,
           aborted ? "user dismiss" : "all frames");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q28L — list+image counterpart to Q28. Identical image cadence (24
// frames × 750 ms × 96×96 stripes/black alternation) and identical
// image position (240, 168), but the compound CREATE pairs the image
// with a 5-item ListObject (verified-rendering shape from Q16/Q17/Q18)
// instead of a TextObject. No per-frame list rebuild — the question
// this probe answers is "does the image half of a list+image compound
// paint during live image-only push?", which is precisely the
// situation Q28 fails. If Q28L paints the image and Q28 doesn't, the
// firmware-side rule is "image children only composite under a List
// parent, not under Text".
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ28LMixedListImageLive() {
  EXT_RAM_BSS_ATTR static char ret[300];
  G2Temple* arm = pickEvenAiArm("imgQ28L");
  if (!arm) {
    snprintf(ret, sizeof(ret), "Img Q28L: no reachable temple");
    return ret;
  }
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic              = allocCreateMagic();
  static constexpr uint32_t kPushSlotStart = G2_MAGIC_IMAGE_BASE + 0x11;  // 227
  static constexpr uint32_t kMaxPushMagic  = 254;
  static constexpr int32_t  kImgW          = 96;
  static constexpr int32_t  kImgH          = 96;
  static constexpr unsigned kFrames        = 24;
  static constexpr uint32_t kPaceMs        = 750;
  static const char* const  kImgChild      = "imgQ28L";
  static const char* const  kListChild     = "lstQ28L";

  // Same magic-cycling math as Q28: 96×96 4-bpp = ~4.7 KB → 2 fragments
  // per push; 28 push magics ÷ 2 = 14 cycling slots before we'd wrap
  // past 255.
  static constexpr unsigned kFragsPerFrame = 2;
  static constexpr unsigned kPushAvailable =
      (unsigned)(kMaxPushMagic - kPushSlotStart + 1);
  static constexpr unsigned kNumSlots = kPushAvailable / kFragsPerFrame;

  probeBanner("Q28L: list+image live (Q28 list-parent counterpart)",
              kCreateMagic, kPushSlotStart, kMaxPushMagic,
              "compound CREATE: 5-item list top + image 96x96 @(240,168). "
              "Same image cadence/position as Q28 but list parent instead "
              "of text. If image renders here but not in Q28, list+image "
              "is the firmware-supported compound. Double-tap to exit.");

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(ret, sizeof(ret), "Img Q28L: pre-burst SHUTDOWN failed");
    return ret;
  }

  // 5-item list — matches the verified-rendering count from Q16/Q17/Q18
  // (runMixedListImageProbe). First row labels the test; rest are
  // filler so we stay safely above any "minimum items" the firmware
  // might enforce (1-item lists are untested).
  const char* listItems[] = {
    "Q28L: list+image live",
    "img: stripes/black alt",
    "row 3",
    "row 4",
    "row 5",
  };
  const size_t listItemCount = sizeof(listItems) / sizeof(listItems[0]);
  // List occupies the same top-half slot Q28 gave its TextObject so the
  // image-position variable stays clean across the comparison.
  const G2ContainerGeom listGeom = { 8, 0, 560, 130 };

  G2ImageTile imgTile = {};
  imgTile.x = 240; imgTile.y = 168;
  imgTile.w = (uint32_t)kImgW;
  imgTile.h = (uint32_t)kImgH;
  imgTile.containerId   = 2;
  imgTile.containerName = kImgChild;

  uint8_t createBuf[1024];
  size_t createLen = g2BuildCreateMixedListImage(
      allocSeq(), kCreateMagic,
      kListChild, listItems, listItemCount, listGeom,
      imgTile, BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(ret, sizeof(ret), "Img Q28L: CREATE-mixed build failed");
    return ret;
  }

  if (!probePrepImageCreateAck(kCreateMagic, arm)) {
    snprintf(ret, sizeof(ret), "Img Q28L: CREATE rejected by lifecycle fence");
    return ret;
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    snprintf(ret, sizeof(ret), "Img Q28L: CREATE fence capture failed");
    return ret;
  }
  if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
    g2CreateTxnRelease();
    snprintf(ret, sizeof(ret), "Img Q28L: CREATE-mixed TX failed");
    return ret;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs,
                               arm, createFence.generation)) {
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret),
             "Img Q28L: CREATE-mixed ack timeout (list+image rejected? "
             "shouldn't happen — Q16/Q17/Q18 use the same builder)");
    return ret;
  }
  DEBUG_G2F("[ImgProbe] Q28L CREATE-mixed acked (list@'%s' + image@'%s' %dx%d)",
            kListChild, kImgChild, (int)kImgW, (int)kImgH);

  // Same alternating stripes/black pair as Q28 — alternation makes a
  // dropped frame eye-visible and matches the Q28 baseline so visual
  // comparison is direct.
  const size_t kBmpCap = 8 * 1024;
  uint8_t* bmpA = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.q28l.bmpA");
  uint8_t* bmpB = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.q28l.bmpB");
  if (!bmpA || !bmpB) {
    if (bmpA) free(bmpA);
    if (bmpB) free(bmpB);
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "Img Q28L: BMP heap alloc failed");
    return ret;
  }
  size_t bmpALen = buildBmp4bpp(bmpA, kBmpCap, kImgW, -kImgH, BMP_PAT_STRIPES);
  size_t bmpBLen = buildBmp4bpp(bmpB, kBmpCap, kImgW, -kImgH, BMP_PAT_ALL_BLACK);
  if (bmpALen == 0 || bmpBLen == 0) {
    free(bmpA); free(bmpB);
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "Img Q28L: BMP build failed");
    return ret;
  }

  unsigned framesPushed = 0;
  unsigned framesFailed = 0;
  const uint32_t startMs = millis();

  for (unsigned i = 0; i < kFrames && !gImgProbeAbort; i++) {
    const uint32_t frameStartMs = millis();
    const uint8_t* bmp    = (i & 1) ? bmpB    : bmpA;
    const size_t   bmpLen = (i & 1) ? bmpBLen : bmpALen;
    const uint32_t pushMagic =
        kPushSlotStart + ((uint32_t)(i % kNumSlots)) * (uint32_t)kFragsPerFrame;

    unsigned okFrags = 0, totalFrags = 0;
    bool pushed = sendImageBmpFragmentsNoCreate(
        *arm, "Q28L", pushMagic,
        /*cid*/ imgTile.containerId, /*cname*/ kImgChild,
        bmp, bmpLen, &okFrags, &totalFrags,
        /*tolerateMissedAcks*/ 3, /*inFlightCapOverride*/ 0,
        G2_IMG_COMPRESS_RAW, nullptr, createFence.generation,
        /*markExpectedEcho*/ false, createFence.lifecycleEpoch);
    if (pushed) {
      framesPushed++;
    } else {
      framesFailed++;
      DEBUG_G2F("[ImgProbe] Q28L frame %u image push FAILED (%u/%u frags)",
                i, okFrags, totalFrags);
    }

    // Deliberately NO list rebuild — Q28's text-rebuild on alternate
    // frames muddied the diagnostic ("which frame numbers are visible?")
    // because rebuild incidentally repaints the compound. Skipping
    // rebuild here means: if the image paints, it does so on every
    // push, independent of any rebuild trigger.

    const uint32_t consumedMs = millis() - frameStartMs;
    if (consumedMs < kPaceMs) {
      const uint32_t remainingMs = kPaceMs - consumedMs;
      const uint32_t kSliceMs = 50;
      const uint32_t sliceEndMs = deadlineAfter(millis(), remainingMs);
      while (deadlinePending(sliceEndMs, millis()) && !gImgProbeAbort) {
        const uint32_t leftMs = sliceEndMs - millis();
        const uint32_t thisMs = leftMs < kSliceMs ? leftMs : kSliceMs;
        vTaskDelay(pdMS_TO_TICKS(thisMs));
      }
    }
  }

  const bool aborted = gImgProbeAbort;
  free(bmpA); free(bmpB);
  probePostProbeShutdown(createFence);
  probeFooter("Q28L", framesPushed, kFrames);

  const uint32_t totalMs = millis() - startMs;
  snprintf(ret, sizeof(ret),
           "Img Q28L: %u/%u img frames (%u fail) in %u ms (end: %s) — "
           "did the image render?",
           framesPushed, kFrames, framesFailed,
           (unsigned)totalMs,
           aborted ? "user dismiss" : "all frames");
  return ret;
}

// ─────────────────────────────────────────────────────────────────────
// Q30 family — 3-pane list+text+image CREATE (unproven schema).
// Evidence so far: list+image paints (Q16/Q28L); list+text paints;
// image+text often blanks (Q28). These probes keep a List sibling and
// add Text+Image with Health-shaped geometry (or Q28L small image).
// ─────────────────────────────────────────────────────────────────────
struct Q30GeomSpec {
  const char* tag;                 // "Q30" / "Q30b" / "Q30c"
  G2ListTextImageOrder order;
  G2ContainerGeom listGeom;
  G2ContainerGeom textGeom;
  int32_t imgX, imgY, imgW, imgH;
  const char* bannerDetail;
};

static const char* runQ30ListTextImageProbe(const Q30GeomSpec& g) {
  EXT_RAM_BSS_ATTR static char ret[320];
  G2Temple* arm = pickEvenAiArm(g.tag);
  if (!arm) {
    snprintf(ret, sizeof(ret), "Img %s: no reachable temple", g.tag);
    return ret;
  }
  G2ImageCreateFence cleanupFence = {};

  // CREATE correlation rotates in the firmware's uint8 domain; Cmd=3 pushes
  // retain their proven 227..254 band.
  const uint32_t kCreateMagic              = allocCreateMagic();
  static constexpr uint32_t kPushSlotStart = G2_MAGIC_IMAGE_BASE + 0x11;  // 227
  static constexpr uint32_t kMaxPushMagic  = 254;
  static constexpr unsigned kFrames        = 24;
  static constexpr uint32_t kPaceMs        = 750;

  char listName[16], txtName[16], imgName[16];
  snprintf(listName, sizeof(listName), "lst%s", g.tag);
  snprintf(txtName, sizeof(txtName), "txt%s", g.tag);
  snprintf(imgName, sizeof(imgName), "img%s", g.tag);

  // Frag estimate for magic cycling (4bpp row stride).
  const unsigned rowStride = (unsigned)(((g.imgW * 4 + 31) / 32) * 4);
  const unsigned pixelBytes = rowStride * (unsigned)g.imgH;
  const unsigned bmpBytes = 14 + 40 + 64 + pixelBytes;
  const unsigned fragsPerFrame = (bmpBytes + G2_IMG_MAPRAW_CHUNK_BYTES - 1) /
                                 G2_IMG_MAPRAW_CHUNK_BYTES;
  const unsigned pushAvailable = (unsigned)(kMaxPushMagic - kPushSlotStart + 1);
  const unsigned numSlots =
      (fragsPerFrame > 0) ? (pushAvailable / fragsPerFrame) : 1;
  if (numSlots < 1) {
    snprintf(ret, sizeof(ret),
             "Img %s: image %dx%d needs %u frags — exceeds uint8 magic room",
             g.tag, (int)g.imgW, (int)g.imgH, fragsPerFrame);
    return ret;
  }

  probeBanner(g.tag, kCreateMagic, kPushSlotStart, kMaxPushMagic,
              g.bannerDetail);

  if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
    snprintf(ret, sizeof(ret), "Img %s: pre-burst SHUTDOWN failed", g.tag);
    return ret;
  }

  // Row 0 is the escape hatch. With a List focused, temple/ring input
  // drives the list — SysEvent double-tap never fires. Any ListEvent
  // CLICK while gImgProbeHoldActive is treated as dismiss (see notify
  // handler); labeling row 0 makes that discoverable.
  const char* listItems[] = {
    "<- Back (exit)",
    "3-pane list+text+img",
    "stable stripes image",
    "text updates live",
    "tap any row to exit",
  };
  const size_t listItemCount = sizeof(listItems) / sizeof(listItems[0]);

  G2TextChildSpec textChild = {};
  textChild.containerId   = 2;
  textChild.containerName = txtName;
  textChild.eventCapture  = false;
  textChild.content       = "Q30 starting...\nTap Back to exit";
  textChild.geom          = g.textGeom;

  G2ImageTile imgTile = {};
  imgTile.x = (uint32_t)g.imgX;
  imgTile.y = (uint32_t)g.imgY;
  imgTile.w = (uint32_t)g.imgW;
  imgTile.h = (uint32_t)g.imgH;
  imgTile.containerId   = 3;
  imgTile.containerName = imgName;

  uint8_t createBuf[1400];
  size_t createLen = g2BuildCreateMixedListTextImage(
      allocSeq(), kCreateMagic,
      listName, listItems, listItemCount, g.listGeom,
      textChild, imgTile, BLOCKS_WIDGET_ID, g.order,
      createBuf, sizeof(createBuf));
  if (createLen == 0) {
    snprintf(ret, sizeof(ret), "Img %s: CREATE-3pane build failed", g.tag);
    return ret;
  }

  if (!probePrepImageCreateAck(kCreateMagic, arm)) {
    snprintf(ret, sizeof(ret), "Img %s: CREATE rejected by lifecycle fence",
             g.tag);
    return ret;
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    snprintf(ret, sizeof(ret), "Img %s: CREATE fence capture failed", g.tag);
    return ret;
  }
  if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
    g2CreateTxnRelease();
    snprintf(ret, sizeof(ret), "Img %s: CREATE-3pane TX failed", g.tag);
    return ret;
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs,
                               arm, createFence.generation)) {
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret),
             "Img %s: CREATE-3pane ack timeout — firmware may reject "
             "list+text+image (ContainerTotalNum=3)",
             g.tag);
    return ret;
  }
  DEBUG_G2F("[ImgProbe] %s CREATE-3pane acked (order=%u, img %dx%d)",
            g.tag, (unsigned)g.order, (int)g.imgW, (int)g.imgH);

  // Arm list-tap dismiss for the whole live window (not only a post-hold).
  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = true;

  const size_t kBmpCap = (size_t)bmpBytes + 256;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.q30.bmp");
  if (!bmp) {
    gImgProbeHoldActive = false;
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "Img %s: BMP heap alloc failed", g.tag);
    return ret;
  }
  // Stable stripes only — alternating stripes/black every 750 ms was a
  // drop-detection trick and looked like a broken flash to operators.
  size_t bmpLen = buildBmp4bpp(bmp, kBmpCap, g.imgW, -g.imgH, BMP_PAT_STRIPES);
  if (bmpLen == 0) {
    free(bmp);
    gImgProbeHoldActive = false;
    probePostProbeShutdown(createFence);
    snprintf(ret, sizeof(ret), "Img %s: BMP build failed", g.tag);
    return ret;
  }

  unsigned framesPushed = 0, framesFailed = 0;
  unsigned textOk = 0, textFail = 0;
  const uint32_t startMs = millis();

  // Push the image once up front, then refresh TEXT on a cadence. Re-push
  // the same stripes every ~5 s as a keep-alive (not a pattern flip).
  // Auto LZ4 bare-block OK on mixed children (Q16d).
  {
    unsigned okFrags = 0, totalFrags = 0;
    bool pushed = sendImageBmpFragmentsNoCreate(
        *arm, g.tag, kPushSlotStart,
        imgTile.containerId, imgName,
        bmp, bmpLen, &okFrags, &totalFrags,
        /*tolerateMissedAcks*/ 3, /*inFlightCapOverride*/ 0,
        G2_IMG_COMPRESS_RAW, nullptr, createFence.generation,
        /*markExpectedEcho*/ false, createFence.lifecycleEpoch);
    if (pushed) framesPushed++;
    else framesFailed++;
  }

  for (unsigned i = 0; i < kFrames && !gImgProbeAbort; i++) {
    const uint32_t frameStartMs = millis();

    char caption[96];
    snprintf(caption, sizeof(caption),
             "%s live\n"
             "tick %u\n"
             "list+text+image\n"
             "stable stripes\n"
             "Tap Back to exit",
             g.tag, i);
    if (sendUpdateTextNamed(*arm, txtName, textChild.containerId, caption))
      textOk++;
    else
      textFail++;

    // Occasional identical image re-push (keep-alive), not a flash pattern.
    if (i > 0 && (i % 6) == 0) {
      const uint32_t pushMagic =
          kPushSlotStart + ((uint32_t)((i / 6) % numSlots)) * (uint32_t)fragsPerFrame;
      unsigned okFrags = 0, totalFrags = 0;
      bool pushed = sendImageBmpFragmentsNoCreate(
          *arm, g.tag, pushMagic,
          imgTile.containerId, imgName,
          bmp, bmpLen, &okFrags, &totalFrags,
          /*tolerateMissedAcks*/ 3, /*inFlightCapOverride*/ 0,
          G2_IMG_COMPRESS_RAW, nullptr, createFence.generation,
          /*markExpectedEcho*/ false, createFence.lifecycleEpoch);
      if (pushed) framesPushed++;
      else framesFailed++;
    }

    const uint32_t consumedMs = millis() - frameStartMs;
    if (consumedMs < kPaceMs) {
      const uint32_t remainingMs = kPaceMs - consumedMs;
      const uint32_t kSliceMs = 50;
      const uint32_t sliceEndMs = deadlineAfter(millis(), remainingMs);
      while (deadlinePending(sliceEndMs, millis()) && !gImgProbeAbort) {
        const uint32_t leftMs = sliceEndMs - millis();
        const uint32_t thisMs = leftMs < kSliceMs ? leftMs : kSliceMs;
        vTaskDelay(pdMS_TO_TICKS(thisMs));
      }
    }
  }

  // If the live window finished without a tap, hold so the operator can
  // inspect the final frame and dismiss via any list row.
  if (!gImgProbeAbort) {
    (void)probeHoldUntilTapOrTimeout(60000);
  }

  const bool aborted = gImgProbeAbort;
  gImgProbeHoldActive = false;
  free(bmp);
  probePostProbeShutdown(createFence);
  probeFooter(g.tag, framesPushed, kFrames > 0 ? kFrames : 1);

  const uint32_t totalMs = millis() - startMs;
  snprintf(ret, sizeof(ret),
           "Img %s: CREATE ok; img pushes %u (%u fail); text %u ok/%u fail; "
           "%u ms (%s) — list+text+img visible?",
           g.tag, framesPushed, framesFailed, textOk, textFail,
           (unsigned)totalMs, aborted ? "user dismiss" : "timeout");
  return ret;
}

const char* g2ProbeImageQ30ListTextImageHealthGeom() {
  // Lens 576×288. Text taller (~5–6 native lines); image sits below it.
  // text y=8 h=112 → ends 120; image y=128 h=144 → ends 272 (16 px margin).
  static const Q30GeomSpec g = {
    /*tag*/ "Q30",
    /*order*/ G2_LTI_ORDER_LIST_TEXT_IMAGE,
    /*listGeom*/ G2_GEOM_SPLIT_LIST,
    /*textGeom*/ G2_GEOM_SPLIT_RIGHT_TOP,
    /*img*/ 280, 128, 288, 144,
    /*banner*/ "3-pane Health geom: list left, tall text top-right (h=112), "
               "image 288x144 @ y=128. Wire order list→text→image. "
               "Stable stripes + live UPDATE_TEXT. Tap any list row to exit.",
  };
  return runQ30ListTextImageProbe(g);
}

const char* g2ProbeImageQ30cListTextSmallImage() {
  static const Q30GeomSpec g = {
    /*tag*/ "Q30c",
    /*order*/ G2_LTI_ORDER_LIST_TEXT_IMAGE,
    /*listGeom*/ G2_GEOM_SPLIT_LIST,
    /*textGeom*/ G2_GEOM_SPLIT_RIGHT_TOP,
    /*img*/ 280, 140, 96, 96,  // under taller text; Q28L-sized tile
    /*banner*/ "3-pane with tall text + 96x96 image @ (280,140). "
               "Falls back if Q30 large tile fails under ContainerTotalNum=3. "
               "Tap any list row to exit.",
  };
  return runQ30ListTextImageProbe(g);
}

// ─────────────────────────────────────────────────────────────────────
// Q21 — full-screen 4-tile (Q12 geometry) with two update rounds logged
// separately: round 0 pushes all four tiles; round 1 re-pushes TL and BR
// with different corner indices so the centre pattern shifts (magic bands
// 211–238 then 239–252, disjoint). Hold afterward like Q12. Does not touch
// production streaming paths — probe-only.
// ─────────────────────────────────────────────────────────────────────
const char* g2ProbeImageQ21LiveFullScreenBurst() {
  EXT_RAM_BSS_ATTR static char ret[280];

  G2Temple* arm = pickEvenAiArm("imgQ21");
  if (!arm) return "Img Q21: no reachable temple";
  G2ImageCreateFence cleanupFence = {};

  const uint32_t kCreateMagic = allocCreateMagic();
  const uint32_t kPushBaseR0[4] = {
      G2_MAGIC_IMAGE_BASE + 0x01,  // 211..217  TL
      G2_MAGIC_IMAGE_BASE + 0x08,  // 218..224  TR
      G2_MAGIC_IMAGE_BASE + 0x0F,  // 225..231  BL
      G2_MAGIC_IMAGE_BASE + 0x16,  // 232..238  BR
  };
  const uint32_t kPushBaseR1[2] = {
      G2_MAGIC_IMAGE_BASE + 0x1F,  // 239..245  TL (round 1)
      G2_MAGIC_IMAGE_BASE + 0x26,  // 246..252  BR (round 1)
  };
  const int32_t kTileW = 288;
  const int32_t kTileH = 144;

  const G2ImageTile kTiles[4] = {
    { 0,                0, (uint32_t)kTileW, (uint32_t)kTileH, 2, "tileTL" },
    { (uint32_t)kTileW, 0, (uint32_t)kTileW, (uint32_t)kTileH, 3, "tileTR" },
    { 0,                (uint32_t)kTileH, (uint32_t)kTileW, (uint32_t)kTileH, 4, "tileBL" },
    { (uint32_t)kTileW, (uint32_t)kTileH, (uint32_t)kTileW, (uint32_t)kTileH, 5, "tileBR" },
  };

  probeBanner("Q21: full-screen 4-tile (2 refresh rounds)",
              kCreateMagic, kPushBaseR0[0], kPushBaseR1[1] + 6,
              "Round 0: paint all 4 tiles like Q12. Round 1: re-push TL+BR "
              "only with shifted markers (239–252 magics). Serial logs show "
              "ms per round — multi-tile sustained refresh vs Q13.");
  if (!probeTearDownActiveContainer(*arm, &cleanupFence))
    return "Img Q21: pre-burst SHUTDOWN failed";

  uint8_t createBuf[256];
  size_t createLen = g2BuildCreateImageMulti(
      allocSeq(), kCreateMagic, kTiles, 4, BLOCKS_WIDGET_ID,
      createBuf, sizeof(createBuf));
  if (createLen == 0) return "Img Q21: CREATE-multi build failed";

  noteOurShutdownSent();
  if (!probePrepImageCreateAck(kCreateMagic, arm)) {
    clearOurShutdownStamp();
    return "Img Q21: CREATE rejected by lifecycle fence";
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    clearOurShutdownStamp();
    return "Img Q21: CREATE fence capture failed";
  }
  if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
    g2CreateTxnRelease();
    probePostProbeShutdown(createFence);
    return "Img Q21: CREATE-multi TX failed";
  }
  if (!probeWaitImageCreateAck(kCreateMagic, kImgCreateAckTimeoutMs,
                               arm, createFence.generation)) {
    probePostProbeShutdown(createFence);
    return "Img Q21: CREATE-multi ack timeout";
  }

  const size_t kBmpCap = 24 * 1024;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.img.q21.bmp");
  if (!bmp) {
    probePostProbeShutdown(createFence);
    return "Img Q21: BMP heap alloc failed";
  }
  const size_t kPixOffset = 14 + 40 + 16 * 4;
  struct CornerXY {
    int32_t x, y;
  };
  const CornerXY kCorner[4] = {
    { kTileW - 24, kTileH - 24 },
    { 0,           kTileH - 24 },
    { kTileW - 24, 0           },
    { 0,           0           },
  };
  const char* kTileTag[4] = { "Q21/TL", "Q21/TR", "Q21/BL", "Q21/BR" };

  unsigned okProbe = 0, fragProbe = 0;
  const uint32_t r0Start = millis();
  for (size_t i = 0; i < 4; i++) {
    size_t bmpLen =
        buildBmp4bpp(bmp, kBmpCap, kTileW, -kTileH, BMP_PAT_STRIPES);
    if (bmpLen == 0) {
      free(bmp);
      probePostProbeShutdown(createFence);
      return "Img Q21: round-0 BMP build failed";
    }
    bmpDrawRect4bpp(bmp + kPixOffset, kTileW, -kTileH,
                    kCorner[i].x, kCorner[i].y, 24, 24, /*idx*/ 15);
    unsigned ok = 0, total = 0;
    (void)sendImageBmpFragmentsNoCreate(
        *arm, kTileTag[i], kPushBaseR0[i], kTiles[i].containerId,
        kTiles[i].containerName, bmp, bmpLen, &ok, &total,
        /*tolerateMissedAcks*/ 0, /*inFlightCapOverride*/ 0,
        G2_IMG_COMPRESS_RAW, nullptr, createFence.generation,
        /*markExpectedEcho*/ false, createFence.lifecycleEpoch);
    okProbe += ok;
    fragProbe += total;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  const uint32_t r0Ms = millis() - r0Start;
  DEBUG_G2F("[ImgProbe] Q21 round0 (4 tiles) wall-clock %u ms, %u/%u frags",
            (unsigned)r0Ms, okProbe, fragProbe);

  const uint32_t r1Start = millis();
  for (size_t pass = 0; pass < 2; pass++) {
    const size_t ti = (pass == 0) ? 0u : 3u;
    size_t bmpLen =
        buildBmp4bpp(bmp, kBmpCap, kTileW, -kTileH, BMP_PAT_STRIPES);
    if (bmpLen == 0) {
      free(bmp);
      probePostProbeShutdown(createFence);
      return "Img Q21: round-1 BMP build failed";
    }
    bmpDrawRect4bpp(bmp + kPixOffset, kTileW, -kTileH,
                    kCorner[ti].x, kCorner[ti].y, 24, 24,
                    /*idx*/ 14);  // different palette index vs round 0
    unsigned ok = 0, total = 0;
    (void)sendImageBmpFragmentsNoCreate(
        *arm, kTileTag[ti], kPushBaseR1[pass], kTiles[ti].containerId,
        kTiles[ti].containerName, bmp, bmpLen, &ok, &total,
        /*tolerateMissedAcks*/ 0, /*inFlightCapOverride*/ 0,
        G2_IMG_COMPRESS_RAW, nullptr, createFence.generation,
        /*markExpectedEcho*/ false, createFence.lifecycleEpoch);
    okProbe += ok;
    fragProbe += total;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  const uint32_t r1Ms = millis() - r1Start;
  DEBUG_G2F("[ImgProbe] Q21 round1 (TL+BR only) wall-clock %u ms (cumulative %u/%u frags)",
            (unsigned)r1Ms, okProbe, fragProbe);

  free(bmp);

  DEBUG_G2F("[ImgProbe] Q21 — pattern up (r0=%u ms r1=%u ms), double-tap to dismiss",
            (unsigned)r0Ms, (unsigned)r1Ms);
  const bool tapped = probeHoldUntilTapOrTimeout(60000);
  probePostProbeShutdown(createFence);
  probeFooter("Q21", okProbe, fragProbe);
  snprintf(ret, sizeof(ret),
           "Img Q21: 4-tile 2 rounds — round0=%u ms round1(TL+BR)=%u ms — %s",
           (unsigned)r0Ms, (unsigned)r1Ms,
           tapped ? "user tap" : "60s timeout");
  return ret;
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
