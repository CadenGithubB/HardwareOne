// =============================================================================
// G2 glasses: image viewers (BMP and JPEG files, camera still and camera stream)
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "System_MemUtil.h"
#include "System_VFS.h"
#include "System_Mutex.h"
#include "G2_HijackCmd.h"

#include "G2_Page_CameraSettings.h"
#include "System_Settings.h"
#include "G2_GlassesInternal.h"

// ─────────────────────────────────────────────────────────────────────
// Async BMP viewer — public API for UI consumers (Files page, future
// gallery, etc.). Mirrors cmd_g2bmp's transport but runs in a worker
// task with a completion callback so a tap dispatcher running on the
// BLE notify thread can launch it without blocking. Holds the image
// until the user double-taps (probe-hold pattern, 60 s safety cap)
// then teardowns and fires onDone so the caller can redraw its page.
// ─────────────────────────────────────────────────────────────────────
struct BmpViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2BmpViewerWorker(void* arg) {
  // Install the paired-user identity + G2 notification source so
  // readBmpFromVfs's guarded reads succeed from this worker task's default
  // ANON TLS slot, and any notify*() fired during the read attributes to
  // "G2 / <user>" rather than "Unknown".
  G2HijackCtxGuard ctxGuard;
  auto* a = (BmpViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAiArm("bmpView");
    if (!arm) {
      DEBUG_G2F("[G2] BMP viewer: no eligible arm");
      break;
    }
    G2ImageCreateFence cleanupFence = {};

    const char* loadErr = "";
    uint8_t* bmp = nullptr;
    size_t bmpLen = 0;
    int32_t bmpW = 0, bmpH = 0;
    if (!readBmpFromVfs(String(a->path), &bmp, &bmpLen, &bmpW, &bmpH, &loadErr)) {
      DEBUG_G2F("[G2] BMP viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }

    // Same magic range as cmd_g2bmp. The probe FSM gates concurrent
    // probes (only one can hold ImageProbing at a time), so reusing the
    // range with the CLI command is safe.
    const unsigned estFrags =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    const uint32_t kCreateMagic   = allocCreateMagic();
    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+
    if (kPushMagicBase + estFrags >= 256) {
      DEBUG_G2F("[G2] BMP viewer: too many fragments (%u) for magic window",
                estFrags);
      free(bmp);
      break;
    }

    DEBUG_G2F("[G2] BMP viewer: '%s' bytes=%u dims=%dx%d frags=%u",
              a->path, (unsigned)bmpLen, (int)bmpW, (int)bmpH, estFrags);

    if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
      DEBUG_G2F("[G2] BMP viewer: pre-push SHUTDOWN failed");
      free(bmp);
      break;
    }

    unsigned okFrags = 0, totalFrags = 0;
    (void)sendImageBmpMultiFragment(*arm, "bmpView",
                                    kCreateMagic, kPushMagicBase,
                                    bmp, bmpLen, &okFrags, &totalFrags);
    free(bmp);

    DEBUG_G2F("[G2] BMP viewer: image up — double-tap to dismiss (60 s cap)");
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] BMP viewer: hold ended via %s (frags %u/%u)",
              tapped ? "user tap" : "60 s timeout", okFrags, totalFrags);

    probePostProbeShutdown(cleanupFence);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;

}

static void g2BmpViewerDrop(void* p) {
  auto* a = (BmpViewerArgs*)p;
  if (!a) return;
  free(a->path);
  delete a;
}

bool g2ShowBmpFile(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  // Heap-low guard — BMP decode buffer can be tens of KB (stack is on g2_session).
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] BMP viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new (std::nothrow) BmpViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  if (!g2SessionSubmit(g2BmpViewerWorker, a, g2BmpViewerDrop, "bmp_view",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] BMP viewer: session enqueue failed");
    g2BmpViewerDrop(a);
    return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────
// Camera viewer — capture one JPEG, decode to RGB888 via
// the shared JPEG HAL, downsample-and-quantize into a 288×144
// 4-bpp grayscale BMP, push using the same single-tile transport as
// g2ShowBmpFile, then hold until the user double-taps. One-shot: no
// auto-refresh and no single-tap recapture (the firmware doesn't emit
// CLICK on image-only state — only DOUBLE_CLICK fires).
//
// Memory profile (peak, all in PSRAM):
//   JPEG copy        ~10–30 KB   (camera dependent, freed after decode)
//   RGB888 buffer    sw·sh·3     (e.g. QVGA 320×240 → 230 KB; freed
//                                 after BMP build)
//   4-bpp BMP        ~21 KB      (held until push completes)
// ─────────────────────────────────────────────────────────────────────

// JPEG decode is shared across chips and independent of a physical camera.
// The HAL chooses acceleration when available and retains a software fallback.
#include "HAL_JPEG.h"

static bool decodeJpegForG2(const uint8_t* bytes, size_t length,
                            hwjpeg::Image& image, const char** error) {
  hwjpeg::DecodeOptions options;
  options.softwareAllocator = [](size_t size) -> uint8_t* {
    return static_cast<uint8_t*>(ps_alloc(size, AllocPref::PreferPsram, "g2.jpeg.rgb"));
  };
  // Hardware DMA requires PSRAM. Honour the existing diagnostic bypass and
  // preserve the shared allocator's fallback/tracing policy on the software path.
  if (psramBypassEnabled()) options.mode = hwjpeg::DecodeMode::SoftwareOnly;
  return hwjpeg::decode(bytes, length, image, options, error);
}

// Build a 288×144 4-bpp top-down grayscale BMP at `out` from a
// `srcW`×`srcH` RGB888 buffer at `src`. Sampling is nearest-neighbour
// (one source pixel per destination pixel) — fast and good enough for
// the 288×144 lens window. Returns total bytes written, or 0 on
// failure (capacity / dim).
//
// `toneMap` (matches gSettings.g2StreamToneMap):
//   Linear   — contrast boost + round quantize (no auto-levels).
//   Balanced — dark-scene percentile stretch + strong contrast (default).
//   Shadows  — dark-scene stretch + contrast + soft bottom lift.
//   Legacy   — old absolute min/max AutoLevels + truncate `>> 4` (A/B).
//
// Auto-levels only run on dim/cramped frames. Stretching a bright scene
// (after AEC settles) remaps midtones downward — "flash bright, then
// dims" — so we leave full-scale frames alone and only punch contrast.
enum class BmpToneMap : uint8_t {
  Linear   = 0,
  Balanced = 1,
  Shadows  = 2,
  Legacy   = 3,
};

static BmpToneMap g2StreamToneMapFromSetting(int v) {
  if (v <= 0) return BmpToneMap::Linear;
  if (v == 1) return BmpToneMap::Balanced;
  if (v == 2) return BmpToneMap::Shadows;
  return BmpToneMap::Legacy;
}

// Integer sqrt for 0..~65k — used as 255*sqrt(x/255) ≈ isqrt(x*255).
static uint32_t g2IsqrtU32(uint32_t n) {
  uint32_t r = 0;
  uint32_t bit = 1u << 30;
  while (bit > n) bit >>= 2;
  while (bit != 0) {
    if (n >= r + bit) {
      n -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

// Contrast around mid-gray. contrastQ8: 256 = 1.0×, 384 = 1.5×, 448 = 1.75×.
static uint8_t g2ApplyContrastU8(uint8_t x, uint16_t contrastQ8) {
  int v = (((int)x - 128) * (int)contrastQ8) / 256 + 128;
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return (uint8_t)v;
}

// Soft lift only below ~96 — recovers crushed shadow detail without
// flattening the whole frame the way a full sqrt expand did.
static uint8_t g2SoftShadowLiftU8(uint8_t x) {
  if (x >= 96) return x;
  const uint32_t gam = g2IsqrtU32((uint32_t)x * 255u);
  const uint32_t w = 96u - (uint32_t)x;  // heavier lift when darker
  return (uint8_t)((((uint32_t)x * (96u - w)) + (gam * w) + 48u) / 96u);
}

static size_t buildBmp4bppFromRgb888(uint8_t* out, size_t outCap,
                                     const uint8_t* src,
                                     int32_t srcW, int32_t srcH,
                                     int32_t dstW = 288,
                                     int32_t dstH = 144,
                                     BmpToneMap toneMap = BmpToneMap::Linear) {
  if (!out || !src) return 0;
  if (srcW <= 0 || srcH <= 0) return 0;
  if (dstW <= 0 || dstH <= 0) return 0;
  const uint32_t rowStride  = ((uint32_t)dstW * 4 + 31) / 32 * 4;  // 4-byte align
  const uint32_t pixelSize  = rowStride * (uint32_t)dstH;
  const uint32_t headerSize = 14 + 40 + 64;
  const uint32_t total      = headerSize + pixelSize;
  if (total > outCap) return 0;

  auto wr16 = [](uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
  };
  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);          p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);  p[3] = (uint8_t)((v >> 24) & 0xff);
  };

  // BITMAPFILEHEADER (14 B)
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2,  total);
  wr16(out + 6,  0);
  wr16(out + 8,  0);
  wr32(out + 10, headerSize);
  // BITMAPINFOHEADER (40 B) — biHeight negative => top-down
  wr32(out + 14, 40);
  wr32(out + 18, (uint32_t)dstW);
  wr32(out + 22, (uint32_t)(-dstH));
  wr16(out + 26, 1);
  wr16(out + 28, 4);
  wr32(out + 30, 0);
  wr32(out + 34, pixelSize);
  wr32(out + 38, 2835);
  wr32(out + 42, 2835);
  wr32(out + 46, 16);
  wr32(out + 50, 0);
  // 16-shade grayscale palette (BGRA)
  for (int i = 0; i < 16; i++) {
    const uint8_t v = (uint8_t)((i * 255) / 15);
    out[54 + i*4 + 0] = v;
    out[54 + i*4 + 1] = v;
    out[54 + i*4 + 2] = v;
    out[54 + i*4 + 3] = 0;
  }

  // Aspect-preserving fit (letterbox). The lens widget is 2:1 (288×144)
  // but every camera framesize is 4:3 or 1:1, so without correction the
  // image gets stretched ~1.5–2× horizontally. Compute the largest
  // fitW×fitH that preserves srcW:srcH and fits inside dstW×dstH, then
  // centre it. Pixels outside the fit region stay zero from the row
  // memset below — palette index 0 is black, so the gaps render as
  // black bars.
  //
  // Aspect comparison via integer cross-multiply to avoid floats:
  //   srcW/srcH  vs  dstW/dstH
  //   →  srcW*dstH  vs  srcH*dstW
  int32_t fitW, fitH;
  if ((int64_t)srcW * dstH > (int64_t)srcH * dstW) {
    // Source aspect wider than destination — width is the bound,
    // letterbox top + bottom.
    fitW = dstW;
    fitH = (int32_t)(((int64_t)srcH * dstW) / srcW);
  } else {
    // Source aspect taller-or-equal — height is the bound, letterbox
    // left + right. (This is the case for every standard camera
    // framesize against the 2:1 lens widget.)
    fitH = dstH;
    fitW = (int32_t)(((int64_t)srcW * dstH) / srcH);
  }
  const int32_t offX = (dstW - fitW) / 2;
  const int32_t offY = (dstH - fitH) / 2;

  // Tone-map pre-pass. Balanced/Shadows: stretch only dim/cramped
  // frames (pHi still low). Bright / full-scale frames keep 0..255 so
  // AEC settling can't get "helped" into a dim remapped look. Legacy:
  // absolute min/max (old behaviour). Linear: contrast-only.
  uint8_t tmMin = 0;
  uint8_t tmMax = 255;
  if (toneMap == BmpToneMap::Balanced || toneMap == BmpToneMap::Shadows ||
      toneMap == BmpToneMap::Legacy) {
    constexpr int kAutoLevelsMinRange = 24;
    constexpr int kDarkSceneLumaMax   = 80;
    constexpr int kDarkSceneMinRange  = 8;
    // If the upper percentile is already near white, the frame has real
    // highlights — do not remapping-stretch (that dims the subject once
    // the sensor AEC stops overexposing the transition frame).
    constexpr int kBrightScenePHiMin  = 200;
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    uint32_t samples = 0;
    uint8_t  absMin  = 255;
    uint8_t  absMax  = 0;
    for (int32_t dy = 0; dy < fitH; dy++) {
      const int32_t sy = (dy * srcH) / fitH;
      const uint8_t* srcRow = src + (size_t)sy * (size_t)srcW * 3;
      for (int32_t dx = 0; dx < fitW; dx++) {
        const int32_t sx = (dx * srcW) / fitW;
        const uint8_t* p = srcRow + (size_t)sx * 3;
        const uint32_t gray = ((uint32_t)p[0] + p[1] + p[2]) / 3;
        const uint8_t g8 = (uint8_t)(gray > 255 ? 255 : gray);
        hist[g8]++;
        samples++;
        if (g8 < absMin) absMin = g8;
        if (g8 > absMax) absMax = g8;
      }
    }
    if (samples > 0) {
      if (toneMap == BmpToneMap::Legacy) {
        // Old absolute min/max AutoLevels (lamp can steal the top).
        const int range = (int)absMax - (int)absMin;
        if (range >= kAutoLevelsMinRange) {
          tmMin = absMin;
          tmMax = absMax;
        }
      } else {
        // Balanced: tighter 5%/95%. Shadows: 2%/98% for soft-lift headroom.
        const uint32_t loDiv = (toneMap == BmpToneMap::Balanced) ? 20u : 50u;
        uint32_t loNeed = samples / loDiv;
        if (loNeed < 1) loNeed = 1;
        uint32_t hiNeed = samples / loDiv;
        if (hiNeed < 1) hiNeed = 1;

        uint8_t pLo = 0, pHi = 255;
        uint32_t cum = 0;
        for (int i = 0; i < 256; i++) {
          cum += hist[i];
          if (cum >= loNeed) { pLo = (uint8_t)i; break; }
        }
        cum = 0;
        for (int i = 255; i >= 0; i--) {
          cum += hist[i];
          if (cum >= hiNeed) { pHi = (uint8_t)i; break; }
        }
        if (pHi < pLo) { pLo = 0; pHi = 255; }

        const int range = (int)pHi - (int)pLo;
        const bool darkScene = (absMax <= (uint8_t)kDarkSceneLumaMax);
        const bool cramped   = (pHi < (uint8_t)kBrightScenePHiMin);
        // Stretch only when the frame is actually short of highlights
        // (or a forced dark-scene recovery). Bright scenes: contrast only.
        if (cramped &&
            (range >= kAutoLevelsMinRange ||
             (darkScene && range >= kDarkSceneMinRange))) {
          tmMin = pLo;
          tmMax = pHi;
        }
      }
    }
  }
  const int tmRange = (int)tmMax - (int)tmMin;

  // Pixel rows are packed RGB888 from the JPEG HAL. For this grayscale
  // conversion the channel order does not matter: we average all three.
  //
  // We track gray range + a coarse sum across the fit region so the
  // caller can log whether the BMP actually has content. If
  // min==max==0 across the whole fit region, the decoder produced a
  // zero buffer (silent decode failure or corrupt input JPEG) and the
  // image will render as solid black on the lens — that's the
  // diagnostic signal we need to distinguish "BMP fine, lens didn't
  // render" from "BMP all-black, fix the capture path."
  uint8_t  diagMin  = 255;
  uint8_t  diagMax  = 0;
  uint64_t diagSum  = 0;
  uint32_t diagSamp = 0;
  uint8_t* dstRowBase = out + headerSize;
  for (int32_t dy = 0; dy < dstH; dy++) {
    uint8_t* dstRow = dstRowBase + (size_t)dy * rowStride;
    memset(dstRow, 0, rowStride);

    // Top + bottom letterbox rows: leave entirely black.
    if (dy < offY || dy >= offY + fitH) continue;

    const int32_t dyFit = dy - offY;
    const int32_t sy = (dyFit * srcH) / fitH;
    const uint8_t* srcRow = src + (size_t)sy * (size_t)srcW * 3;

    // Sample only the fit-region columns; everything outside stays
    // black from the memset above.
    const int32_t dxStart = offX;
    const int32_t dxEnd   = offX + fitW;
    for (int32_t dx = dxStart; dx < dxEnd; dx++) {
      const int32_t dxFit = dx - offX;
      const int32_t sx = (dxFit * srcW) / fitW;
      const uint8_t* p = srcRow + (size_t)sx * 3;
      // Luma approximation — straight average is fine for monochrome
      // lens; weighted ITU-R BT.601 (0.299 R + 0.587 G + 0.114 B) is
      // marginally better but adds a 16-bit multiply per pixel.
      const uint32_t gray = ((uint32_t)p[0] + p[1] + p[2]) / 3;
      const uint8_t  gray8 = (uint8_t)(gray > 255 ? 255 : gray);
      if (gray8 < diagMin) diagMin = gray8;
      if (gray8 > diagMax) diagMax = gray8;
      diagSum  += gray8;
      diagSamp += 1;
      // Remap [tmMin..tmMax] -> [0..255]. When auto-levels is off or the
      // pre-pass found the range too small to stretch safely, tmMin=0
      // and tmMax=255 so this is a no-op.
      uint32_t mapped;
      if (tmRange > 0) {
        int v = (int)gray8 - (int)tmMin;
        if (v < 0) v = 0;
        mapped = ((uint32_t)v * 255U) / (uint32_t)tmRange;
        if (mapped > 255) mapped = 255;
      } else {
        mapped = gray8;
      }

      uint8_t nibble;
      if (toneMap == BmpToneMap::Legacy) {
        // Old path: truncate to nibble (crushes 0..15 → 0). Kept for A/B.
        nibble = (uint8_t)(mapped >> 4);
      } else {
        // Contrast punch so neighboring greens land in different bins.
        // Balanced is punchiest; Shadows adds a soft bottom lift only.
        const uint16_t contrastQ8 =
            (toneMap == BmpToneMap::Balanced) ? 448u :  // 1.75×
            (toneMap == BmpToneMap::Shadows)  ? 384u :  // 1.50×
                                               352u;  // Linear 1.375×
        uint8_t toned = g2ApplyContrastU8((uint8_t)mapped, contrastQ8);
        if (toneMap == BmpToneMap::Shadows) {
          toned = g2SoftShadowLiftU8(toned);
        }
        nibble = (uint8_t)(((uint32_t)toned * 15u + 127u) / 255u);  // round
      }
      // 4-bpp packing: high nibble = even column, low nibble = odd
      const uint32_t byteIdx = (uint32_t)dx >> 1;
      if ((dx & 1) == 0) {
        dstRow[byteIdx] |= (uint8_t)(nibble << 4);
      } else {
        dstRow[byteIdx] |= nibble;
      }
    }
  }
  // Coarse content stat — printed by the worker so the user can tell
  // an "all-black" BMP (decode failure) from a successfully-built one.
  // Empty fit region (shouldn't happen, defensive) → leave defaults.
  const uint32_t mean = diagSamp ? (uint32_t)(diagSum / diagSamp) : 0;
  DEBUG_G2F("[G2] BMP build: src=%dx%d → fit=%dx%d offX=%d offY=%d "
            "gray min=%u max=%u mean=%u samples=%u",
            srcW, srcH, fitW, fitH, offX, offY,
            (unsigned)diagMin, (unsigned)diagMax, (unsigned)mean,
            (unsigned)diagSamp);
  return total;
}

#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP.h"    // captureFrame, cameraWidth, cameraHeight, gCameraRunning
#include "System_Camera_Video.h"  // videoRecording, start/stopVideoRecording — stream Rec row

struct CameraViewerArgs {
  void (*onDone)();
};

static void g2CameraViewerWorker(void* arg) {
  auto* a = (CameraViewerArgs*)arg;
  uint8_t* jpegBuf = nullptr;
  hwjpeg::Image decoded;
  uint8_t* bmpBuf  = nullptr;

  do {
    if (!gCameraRunning) {
      DEBUG_G2F("[G2] Camera viewer: camera not enabled — abort");
      break;
    }
    G2Temple* arm = pickEvenAiArm("camView");
    if (!arm) {
      DEBUG_G2F("[G2] Camera viewer: no eligible arm");
      break;
    }
    G2ImageCreateFence cleanupFence = {};

    // Capture. captureFrame() copies into PSRAM and returns ownership.
    size_t jpegLen = 0;
    jpegBuf = captureFrame(&jpegLen);
    if (!jpegBuf || jpegLen == 0) {
      DEBUG_G2F("[G2] Camera viewer: capture failed");
      break;
    }
    const char* decodeError = nullptr;
    if (!decodeJpegForG2(jpegBuf, jpegLen, decoded, &decodeError)) {
      DEBUG_G2F("[G2] Camera viewer: JPEG decode failed: %s", decodeError);
      break;
    }
    const int srcW = decoded.width;
    const int srcH = decoded.height;
    free(jpegBuf); jpegBuf = nullptr;

    // Build 288×144 4-bpp BMP. Header (118 B) + 144*144 px-bytes ≈ 21 KB.
    const size_t kBmpCap = 14 + 40 + 64 + (288 / 2) * 144 + 64;
    bmpBuf = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.cam.bmp");
    if (!bmpBuf) {
      DEBUG_G2F("[G2] Camera viewer: bmp alloc failed");
      break;
    }
    const size_t bmpLen = buildBmp4bppFromRgb888(bmpBuf, kBmpCap, decoded.pixels, srcW, srcH);
    decoded.reset();
    if (bmpLen == 0) {
      DEBUG_G2F("[G2] Camera viewer: bmp build failed");
      break;
    }

    // Keep the Cmd=3 push fragments in +0x11..+0x1F. CREATE correlation
    // rotates independently. The frame is single-fragment but retains room
    // for safety.
    const uint32_t kCreateMagic   = allocCreateMagic();
    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x11;  // 227+
    if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
      DEBUG_G2F("[G2] Camera viewer: pre-push SHUTDOWN failed");
      break;
    }

    unsigned okFrags = 0, totalFrags = 0;
    (void)sendImageBmpMultiFragment(*arm, "camView",
                                    kCreateMagic, kPushMagicBase,
                                    bmpBuf, bmpLen,
                                    &okFrags, &totalFrags);
    DEBUG_G2F("[G2] Camera viewer: image up (%u/%u frags) — double-tap to dismiss",
              okFrags, totalFrags);
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] Camera viewer: hold ended via %s",
              tapped ? "user tap" : "60 s timeout");

    probePostProbeShutdown(cleanupFence);
  } while (0);

  if (jpegBuf) free(jpegBuf);
  decoded.reset();
  if (bmpBuf)  free(bmpBuf);

  // Camera stays running across the dismiss — user explicitly chose
  // ON via the toggle and expects subsequent captures to work without
  // re-toggling. (Earlier auto-stop here was too aggressive: it fixed
  // the QVGA+ post-viewer DRAM exhaustion but broke the common-case
  // flow at smaller framesizes where there's plenty of DRAM. If
  // QVGA+ hits xTaskCreate failure on the post-viewer page-swap
  // again, fix the underlying issue (persistent worker task /
  // smaller stack / lower FB count for ImageProbing path) rather
  // than yanking the user's camera out from under them.)

  if (a && a->onDone) a->onDone();
  delete a;

}

static void g2CameraViewerDrop(void* p) { delete (CameraViewerArgs*)p; }

bool g2ShowCameraViewer(void (*onDone)()) {
  if (!gCameraRunning) {
    DEBUG_G2F("[G2] Camera viewer: declining — camera not enabled");
    return false;
  }
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] Camera viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new (std::nothrow) CameraViewerArgs;
  if (!a) return false;
  a->onDone = onDone;
  if (!g2SessionSubmit(g2CameraViewerWorker, a, g2CameraViewerDrop, "cam_view",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] Camera viewer: session enqueue failed");
    g2CameraViewerDrop(a);
    return false;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────
// Camera stream — same capture+push pipeline as g2ShowCameraViewer
// but in a loop. We tear the active container down once at the
// start (Hijacked → ImageProbing transition), then for each frame
// capture, decode, build the BMP, and push via
// sendImageBmpMultiFragment with the same magic range each time —
// the firmware accepts a fresh CREATE that replaces the previous
// image session. Loop stops when the user double-taps (firmware
// emits SysEvent DOUBLE_CLICK; the existing probe-hold hook flips
// gImgProbeHoldTapPending which we poll between frames).
//
// Effective frame rate is bounded by the BLE push time —
// ~2.7 s/frame at 288×144 4-bpp = 7 frags. With the ~100 ms
// breathing delay between frames the cadence is ~0.35 fps.
// ─────────────────────────────────────────────────────────────────────

struct CameraStreamArgs {
  void (*onDone)();
};

// User-configurable lens-stream dimensions. The worker snapshots
// these at session start (so changing mid-stream takes effect on the
// next stream) and does its own clamp. Bounds match the lens panel
// (288×144) and a 16 px floor below which the firmware reassembler
// gets unhappy. Width is forced even because 4-bpp packs 2 px/byte.
//
// Source of truth is gSettings.g2StreamWidth/Height (persisted, default
// 96×96). The stream worker reads gSettings directly each session.
// Reference cadence: 96×96 → ~1.4 fps; 160×120 → ~0.7 fps;
// 288×144 → ~0.37 fps.

static void g2CameraStreamWorker(void* arg) {
  auto* a = (CameraStreamArgs*)arg;
  // Set when the user taps the "Settings >>" row — controls whether the
  // post-loop cleanup invokes onDone() (Back path → CAM detail) or
  // chains into g2ShowCameraSettingsMenu() (Settings path → settings
  // page → relaunch on back via g2CamStreamSettingsExitRelaunch).
  bool exitToSettings = false;

  do {
    if (!gCameraRunning) {
      DEBUG_G2F("[G2] Camera stream: camera not enabled — abort");
      break;
    }
    G2Temple* arm = pickEvenAiArm("camStream");
    if (!arm) {
      DEBUG_G2F("[G2] Camera stream: no eligible arm");
      break;
    }
    G2ImageCreateFence cleanupFence = {};

    // One-time SHUTDOWN of the underlying container (the Sensors
    // detail list). Subsequent frames just CREATE/push within the
    // ImageProbing FSM state.
    if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
      DEBUG_G2F("[G2] Camera stream: pre-stream SHUTDOWN failed");
      break;
    }

    // Stream is now list+image compound (Q28L's verified shape) — the
    // image lives in the bottom half, a 4-row list controls input in
    // the top half. The list-tap dispatcher in the BLE notify handler
    // sets bits in gCamStreamPendingTap which we drain each loop.
    // Double-tap-to-dismiss is intentionally NOT armed here — exit is
    // via the "<- Back" list row instead. The image-probe abort signal
    // is still cleared so a stale flag from a prior probe doesn't
    // immediately terminate this stream (firmware safety-timeout still
    // sets it via g2LensApplyHijackActive — see fix from 2026-05-07).
    gImgProbeHoldTapPending = false;
    gImgProbeAbort          = false;
    gImgProbeHoldActive     = false;
    gCamStreamPendingTap    = 0;
    // Don't inherit a stale relaunch flag from a prior session — fresh
    // entry should always honour onDone() unless THIS run sets it.
    g2CamStreamSettingsExitRelaunch = false;

    DEBUG_G2F("[G2] Camera stream: started (list+image; tap row to act)");

    // CREATE once on frame 0; subsequent frames push into the same
    // container with sendImageBmpFragmentsNoCreate. Re-issuing CREATE
    // every frame with the same magic/CID is silently dropped by the
    // firmware, which is what was killing the stream after 1 frame.
    //
    // Stream dims are user-configurable (see gSettings.g2StreamWidth/Height).
    // Smaller dims = fewer Cmd=3 fragments per push = higher fps. Reference
    // points (4-bpp, G2_IMG_MAPRAW_CHUNK_BYTES):
    //   ·  96× 96 →  ~4.8 KB →  2 frags →  ~700 ms /frame  (~1.4 fps)
    //   · 160×120 →  ~9.7 KB →  3 frags → ~1100 ms /frame  (~0.9 fps)
    //   · 288×144 → ~20.9 KB →  6 frags → ~2300 ms /frame  (~0.43 fps)
    // Confirmed by Q19 probe that solo small-dim BMPs render on the
    // lens; 288×144 is the lens-panel native size.
    int32_t streamW = gSettings.g2StreamWidth;
    int32_t streamH = gSettings.g2StreamHeight;
    if (streamW < 16)  streamW = 16;
    if (streamH < 16)  streamH = 16;
    if (streamW > 288) streamW = 288;
    if (streamH > 144) streamH = 144;
    streamW &= ~1;  // even — 4-bpp packs 2 px/byte

    // Estimate fragsPerFrame from the BMP layout we'll build. The
    // 4-bpp row stride is 4-byte aligned (matches buildBmp4bppFromRgb888).
    const size_t   kChunkBytes      = G2_IMG_MAPRAW_CHUNK_BYTES;
    const size_t   kBmpHdrBytes     = 14 + 40 + 64;
    const size_t   bmpRowStride     = (((size_t)streamW * 4 + 31) / 32) * 4;
    const size_t   bmpEstLen        = kBmpHdrBytes + bmpRowStride * (size_t)streamH;
    const unsigned fragsPerFrame    = (unsigned)((bmpEstLen + kChunkBytes - 1) / kChunkBytes);

    // Push magics must all fit in uint8 ≤ 255 (firmware constraint —
    // see Q10 comment). We have 227..254 = 28 magics for
    // push fragments. Each frame's burst occupies fragsPerFrame
    // contiguous magics, so we get floor(28 / fragsPerFrame) cycling
    // slots — e.g. 14 @2 frags, 9 @3, 7 @4, 4 @6–7 frags. Cycling
    // avoids reusing the same magic range on consecutive frames,
    // which would confuse the firmware reassembler.
    const uint32_t kPushSlotStart   = G2_MAGIC_IMAGE_BASE + 0x11;  // 227
    const uint32_t kMaxPushMagic    = 254;
    const unsigned pushAvailable    = (unsigned)(kMaxPushMagic - kPushSlotStart + 1);
    const unsigned numSlots         = (fragsPerFrame > 0 && fragsPerFrame <= pushAvailable)
                                        ? (pushAvailable / fragsPerFrame)
                                        : 1;
    // BMP cap sized for the lens-panel max so the per-frame ps_alloc
    // is a no-op regardless of the configured stream dims.
    const size_t   kBmpCap              = 14 + 40 + 64 + (288 / 2) * 144 + 64;
    const uint32_t kStreamSafetyCapMs   = 5UL * 60UL * 1000UL;  // 5 minute cap
    // Defensive frame-rate floor from cameraStreamFps (same setting as
    // web poll + AVI record). BLE push usually dominates, so raising
    // FPS mostly helps small stream dims / fast links and recording
    // when started from Rec. Matches camerafps 1..20 → 1000..50 ms.
    int fpsTarget = gSettings.cameraStreamFps;
    if (fpsTarget < 1) fpsTarget = 5;
    if (fpsTarget > 20) fpsTarget = 20;
    const uint32_t kMinFramePeriodMs = 1000u / (uint32_t)fpsTarget;

    // Image lives in the bottom half, list lives in the top half.
    // List geom matches the verified-rendering Q28L shape so we know
    // the firmware composites both children. Image position centers
    // streamW × streamH inside the bottom 144 px band; W is forced
    // even (4-bpp packing constraint).
    const G2ContainerGeom kListGeom = { 8, 0, 560, 130 };
    const int32_t imgX = ((576 - streamW) / 2) & ~1;
    const int32_t imgY = 144 + (144 - streamH) / 2;
    G2ImageTile imgTile = {};
    imgTile.x = (uint32_t)imgX;
    imgTile.y = (uint32_t)imgY;
    imgTile.w = (uint32_t)streamW;
    imgTile.h = (uint32_t)streamH;
    imgTile.containerId   = 2;
    imgTile.containerName = "imgCam";

    // Root row labels — kept short so the CREATE-list pb stays well
    // under the 240 B single-fragment cap. Labels stay static for the
    // whole stream: REBUILD-list mid-compound drops the image child
    // (verified 2026-05-08). Snapshot / Record act silently; logs +
    // SD files are the feedback path. Record row text is a toggle
    // affordance (start when idle, stop when videoRecording).
    static const char* const kRootRows[] = {
      "<- Back",
      "Snapshot",
      "Rec start/stop",
      "Settings >>",
    };
    static constexpr size_t kRootRowCount = 4;
    G2ImageCreateFence streamFence = {};

    // Build + send CREATE-mixed-list-image. We do this by hand (rather
    // than via sendImageBmpMultiFragment) because the helper hardcodes
    // the image-only CREATE path. The image-push helpers below
    // (sendImageBmpFragmentsNoCreate) target this CREATE's image child.
    {
      // PSRAM: BLE envelope staging — see the g2_map_page twin above for the
      // rationale. Single worker task (g2_cam_stream) so static is safe; its own
      // buffer because it can run concurrently with the map worker.
      EXT_RAM_BSS_ATTR static uint8_t createBuf[1024];
      const uint32_t createMagic = allocCreateMagic();
      size_t createLen = g2BuildCreateMixedListImage(
          allocSeq(), createMagic,
          /*listName*/ "lstCam",
          kRootRows, kRootRowCount, kListGeom,
          imgTile, BLOCKS_WIDGET_ID,
          createBuf, sizeof(createBuf));
      if (createLen == 0) {
        DEBUG_G2F("[G2] Camera stream: CREATE-mixed build failed");
        break;
      }
      if (!probePrepImageCreateAck(createMagic, arm)) {
        DEBUG_G2F("[G2] Camera stream: CREATE rejected by lifecycle fence");
        break;
      }
      if (!captureImageCreateFence(*arm, &streamFence)) {
        g2CreateTxnRelease();
        DEBUG_G2F("[G2] Camera stream: CREATE fence capture failed");
        break;
      }
      if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
        DEBUG_G2F("[G2] Camera stream: CREATE-mixed TX failed");
        g2CreateTxnRelease();
        break;
      }
      if (!probeWaitImageCreateAck(createMagic, kImgCreateAckTimeoutMs,
                                   arm, streamFence.generation)) {
        DEBUG_G2F("[G2] Camera stream: CREATE-mixed ack timeout");
        probePostProbeShutdown(streamFence);
        break;
      }
      DEBUG_G2F("[G2] Camera stream: CREATE-mixed acked "
                "(list@'lstCam' + image@'imgCam' %dx%d @(%d,%d))",
                (int)streamW, (int)streamH, (int)imgX, (int)imgY);
    }

    // Mark active so the BLE ListEvent dispatcher routes lstCam taps
    // into our pending-tap bitfield. Cleared at exit so late echoes
    // don't pollute a future stream session.
    gCamStreamActive = true;

    DEBUG_G2F("[G2] Camera stream: dims=%dx%d frags/frame=%u slots=%u cap=%uB",
              (int)streamW, (int)streamH, fragsPerFrame, numSlots,
              (unsigned)kBmpCap);

    int      frame = 0;
    uint32_t startMs = millis();
    // Set when the worker should save the next captured JPEG. The
    // user's tap can land at any moment; defer the save until we have
    // a fresh JPEG in hand on the next iteration so we never race a
    // half-built buffer.
    bool snapshotPending = false;

    while (true) {
      // Safety: never let the stream run indefinitely.
      if ((millis() - startMs) > kStreamSafetyCapMs) {
        DEBUG_G2F("[G2] Camera stream: 5-min safety cap reached, stopping");
        break;
      }
      // Firmware-side hijack timeout (or peer-side dismiss) tears down
      // lens-page mode. Catch that here so we don't spend a frame's
      // worth of capture+decode+BMP-build on a panel no one's looking at.
      if (!g2LensGetState().hijackActive) {
        DEBUG_G2F("[G2] Camera stream: hijack ended mid-stream, stopping");
        break;
      }
      // Firmware-driven abort (safety-timeout sets gImgProbeAbort via
      // g2LensApplyHijackActive). User-side dismiss now flows through
      // gCamStreamPendingTap instead, but the abort flag is still our
      // emergency stop for non-user causes.
      if (gImgProbeAbort) {
        DEBUG_G2F("[G2] Camera stream: gImgProbeAbort set, stopping");
        break;
      }

      // Drain pending list-row taps. Read-and-clear so a re-tap during
      // this iteration sets a fresh pending bit for the next pass.
      const uint32_t taps = gCamStreamPendingTap;
      gCamStreamPendingTap = 0;
      if (taps & 0x1u) {
        DEBUG_G2F("[G2] Camera stream: Back tap → exit");
        break;
      }
      if (taps & 0x8u) {
        DEBUG_G2F("[G2] Camera stream: Settings tap → exit, chain to settings");
        exitToSettings = true;
        break;
      }
      if (taps & 0x2u) {
        // Defer: the actual save happens after the next captureFrame()
        // succeeds, while we still own the JPEG buffer.
        snapshotPending = true;
      }
      if (taps & 0x4u) {
        // Same CLI path as the web Sensors Record button. Keep the
        // viewfinder running — recorder and this worker share
        // captureFrame() (non-blocking mutex); losers skip a frame.
        if (videoRecording) {
          stopVideoRecording();
          DEBUG_G2F("[G2] Camera stream: recording stopped (%s, %u frames)",
                    videoLastRecordingPath(),
                    (unsigned)videoLastRecordingFrames());
        } else if (!VFS::isSdWritable()) {
          DEBUG_G2F("[G2] Camera stream: record ignored — SD not writable");
        } else if (startVideoRecording()) {
          DEBUG_G2F("[G2] Camera stream: recording started");
        } else {
          DEBUG_G2F("[G2] Camera stream: recording start failed");
        }
      }

      const uint32_t frameStartMs = millis();

      // Capture
      size_t   jpegLen = 0;
      uint8_t* jpegBuf = captureFrame(&jpegLen);
      if (!jpegBuf || jpegLen == 0) {
        // While recording, the AVI task often wins the camera mutex —
        // treat as a dropped viewfinder frame, not a fatal stream error.
        if (jpegBuf) free(jpegBuf);
        if (videoRecording) {
          DEBUG_G2F("[G2] Camera stream: capture busy (frame %d, recording) — skip",
                    frame);
          vTaskDelay(pdMS_TO_TICKS(50));
          continue;
        }
        DEBUG_G2F("[G2] Camera stream: capture failed (frame %d)", frame);
        break;
      }
      // Snapshot save — has to land BEFORE we free(jpegBuf). We save
      // the full-resolution JPEG (not the downsampled BMP we push to
      // lens) so the user gets a usable photo, not a 96×96 4-bpp
      // postage stamp. Filename: cam_<millis>.jpg under /sd/PICTURES/.
      // Save is silent on the lens (no row swap) — see the firmware-
      // killed-image note up by kRootRows. The DEBUG log line + the
      // file appearing under /sd/PICTURES/ are the feedback path.
      if (snapshotPending) {
        snapshotPending = false;
        char path[64];
        snprintf(path, sizeof(path), "/sd/PICTURES/cam_%lu.jpg",
                 (unsigned long)millis());
        // Route through VFS (shared FsLockGuard + permission guard) instead of
        // raw POSIX fopen — fopen also requires /sd to be registered as a POSIX
        // mount, whereas VFS dispatches /sd to the Arduino SD backend the rest
        // of the camera code already uses. Behavior is otherwise unchanged: the
        // parent dir must still pre-exist (open does not create it).
        FsLockGuard fsGuard("g2.cam_snapshot.write");
        File f = VFS::openGuarded(String(path), "w",
                                  VFS::systemAuth(VFS::Scopes::PICTURES_SD, "g2.cam_snapshot"), true);
        if (f) {
          const size_t wrote = f.write((const uint8_t*)jpegBuf, jpegLen);
          f.close();
          if (wrote == jpegLen) {
            DEBUG_G2F("[G2] Camera stream: snapshot saved %s (%u B)",
                      path, (unsigned)jpegLen);
          } else {
            DEBUG_G2F("[G2] Camera stream: snapshot write short %u/%u to %s",
                      (unsigned)wrote, (unsigned)jpegLen, path);
          }
        } else {
          DEBUG_G2F("[G2] Camera stream: snapshot open failed: %s "
                    "(SD mounted? /sd/PICTURES/ exists?)",
                    path);
        }
      }

      hwjpeg::Image decoded;
      const char* decodeError = nullptr;
      if (!decodeJpegForG2(jpegBuf, jpegLen, decoded, &decodeError)) {
        DEBUG_G2F("[G2] Camera stream: JPEG decode failed (frame %d): %s", frame, decodeError);
        free(jpegBuf);
        break;
      }
      free(jpegBuf);
      const int srcW = decoded.width;
      const int srcH = decoded.height;

      // Build BMP
      uint8_t* bmpBuf = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.camstr.bmp");
      if (!bmpBuf) {
        DEBUG_G2F("[G2] Camera stream: bmp alloc failed");
        break;
      }
      const BmpToneMap toneMap =
          g2StreamToneMapFromSetting(gSettings.g2StreamToneMap);
      const size_t bmpLen = buildBmp4bppFromRgb888(bmpBuf, kBmpCap,
                                                   decoded.pixels, srcW, srcH,
                                                   streamW, streamH,
                                                   toneMap);
      decoded.reset();
      if (bmpLen == 0) {
        DEBUG_G2F("[G2] Camera stream: bmp build failed (frame %d)", frame);
        free(bmpBuf);
        break;
      }

      // Push fragments only (CREATE was done once above). Magic
      // cycling avoids re-using the same range on consecutive frames
      // — see numSlots derivation above. tolerateMissedAcks=3 keeps
      // the stream flowing through occasional ack drops; streaming is
      // forgiving since frame N+1 overwrites N quickly.
      const unsigned kStreamAckTolerance = 3;
      unsigned okFrags = 0, totalFrags = 0;
      const uint32_t pushBase =
          kPushSlotStart + (uint32_t)(frame % numSlots) * (uint32_t)fragsPerFrame;
      const bool pushed = sendImageBmpFragmentsNoCreate(
          *arm, "camStream",
          pushBase, /*cid*/ imgTile.containerId,
          /*cname*/ imgTile.containerName,
          bmpBuf, bmpLen, &okFrags, &totalFrags,
          kStreamAckTolerance, /*inFlightCapOverride*/ 0,
          G2_IMG_COMPRESS_RAW, nullptr,
          streamFence.generation,
          /*markExpectedEcho*/ false,
          streamFence.lifecycleEpoch);
      free(bmpBuf);

      if (!pushed) {
        DEBUG_G2F("[G2] Camera stream: push aborted (frame %d, %u/%u frags)",
                  frame, okFrags, totalFrags);
        break;
      }

      DEBUG_G2F("[G2] Camera stream: frame %d pushed (%u frags)",
                frame, totalFrags);
      frame++;

      // Per-frame floor from cameraStreamFps. When BLE push already
      // exceeds that period (typical at larger stream dims), the small
      // post-burst breather lets FreeRTOS run idle/heartbeat tasks.
      const uint32_t elapsedMs = millis() - frameStartMs;
      const uint32_t toSleepMs = (elapsedMs < kMinFramePeriodMs)
                                   ? (kMinFramePeriodMs - elapsedMs)
                                   : 25;
      vTaskDelay(pdMS_TO_TICKS(toSleepMs));
    }

    gCamStreamActive     = false;
    gCamStreamPendingTap = 0;
    DEBUG_G2F("[G2] Camera stream: stopped after %d frame(s) (%s)",
              frame,
              exitToSettings           ? "Settings tap" :
              !g2LensGetState().hijackActive      ? "hijack ended (safety-timeout/peer)" :
              gImgProbeAbort           ? "abort flag"
                                       : "Back tap or loop exit");
    probePostProbeShutdown(streamFence.arm ? streamFence : cleanupFence);
  } while (0);

  // Camera stays running across the stream-stop. See the equivalent
  // comment in g2CameraViewerWorker: auto-stopping the camera after
  // a lens action is bad UX — the user toggled CAM ON and expects
  // it to stay on until they toggle OFF.

  if (exitToSettings) {
    // Settings path — chain into the camera-settings page. The page's
    // back-row handler checks g2CamStreamSettingsExitRelaunch and
    // relaunches THIS worker on back, returning the user to the live
    // stream with their settings already applied. onDone is NOT
    // invoked here — that's only the Back-row exit.
    g2CamStreamSettingsExitRelaunch = true;
    g2ShowCameraSettingsMenu();
  } else if (a && a->onDone) {
    // Back-row exit (or any non-Settings stop) → invoke onDone, which
    // returns the user to the CAM detail page (per the Sensors-detail
    // wire-up at G2_Page_Sensors.cpp:930).
    a->onDone();
  }
  delete a;

}

static void g2CameraStreamDrop(void* p) { delete (CameraStreamArgs*)p; }

bool g2ShowCameraStream(void (*onDone)()) {
  if (!gCameraRunning) {
    DEBUG_G2F("[G2] Camera stream: declining — camera not enabled");
    return false;
  }
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] Camera stream: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new (std::nothrow) CameraStreamArgs;
  if (!a) return false;
  a->onDone = onDone;
  if (!g2SessionSubmit(g2CameraStreamWorker, a, g2CameraStreamDrop, "cam_stream",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] Camera stream: session enqueue failed");
    g2CameraStreamDrop(a);
    return false;
  }
  return true;
}

#else  // !ENABLE_CAMERA_SENSOR
bool g2ShowCameraViewer(void (*onDone)()) {
  (void)onDone;
  return false;
}
bool g2ShowCameraStream(void (*onDone)()) {
  (void)onDone;
  return false;
}
#endif

// ─────────────────────────────────────────────────────────────────────
// Full-screen BMP viewer — 288×144 source upscaled 2× via nearest-
// neighbour to a 576×288 image and shipped as a 2×2 grid of 288×144
// tiles. The single-large-image transport (one ImageObject with
// 576×288 geom) has never been verified on this firmware; Q12 is the
// known-good path for full-canvas content, so we reuse its tile shape.
//
// Memory: 1× source BMP (~21 KB PSRAM) + 1× tile-build buffer (~21 KB
// PSRAM, reused across the four pushes). The upscale is done per-tile
// directly into the build buffer so we never materialise the full
// 576×288 intermediate.
// ─────────────────────────────────────────────────────────────────────

// Read 288×144 4bpp BMP `src` (raw file as returned by readBmpFromVfs)
// and emit a fresh 288×144 4bpp BMP into `out` whose pixel data is the
// `quadrant`-th quarter of `src` upscaled 2× by pixel duplication.
//   quadrant 0 = top-left  (src cols 0..143,  rows 0..71)
//            1 = top-right (src cols 144..287, rows 0..71)
//            2 = bottom-left
//            3 = bottom-right
// Output BMP is always top-down (negative biHeight) so caller-side
// row math doesn't have to flip. Returns total bytes written, or 0 on
// failure (capacity / malformed source).
static size_t buildTileBmpFromQuadrant(uint8_t* out, size_t outCap,
                                       const uint8_t* src, size_t srcLen,
                                       uint8_t quadrant) {
  if (!out || !src || quadrant > 3) return 0;
  if (outCap < 70 || srcLen < 118) return 0;
  if (!(src[0] == 'B' && src[1] == 'M')) return 0;

  // Source pixel-data offset from the file header (bfOffBits).
  const uint32_t srcPxOff = (uint32_t)src[10] | ((uint32_t)src[11] << 8) |
                            ((uint32_t)src[12] << 16) | ((uint32_t)src[13] << 24);
  if (srcPxOff < 14 + 40 + 64 || srcPxOff > srcLen) return 0;

  // Source dimensions (must be 288×144 — readBmpFromVfs already
  // enforced this, but we re-check so this helper is self-defensive).
  const int32_t srcW = (int32_t)((uint32_t)src[18] | ((uint32_t)src[19] << 8) |
                                 ((uint32_t)src[20] << 16) | ((uint32_t)src[21] << 24));
  const int32_t srcHsigned = (int32_t)((uint32_t)src[22] | ((uint32_t)src[23] << 8) |
                                       ((uint32_t)src[24] << 16) | ((uint32_t)src[25] << 24));
  if (srcW != 288) return 0;
  const int32_t srcH = (srcHsigned < 0) ? -srcHsigned : srcHsigned;
  if (srcH != 144) return 0;
  const bool srcTopDown = (srcHsigned < 0);

  // Output BMP: 288×144 top-down 4bpp. Header layout matches
  // buildBmp4bpp() so the wire transport doesn't need to know the
  // difference between a synthetic test-pattern tile and a
  // user-content tile. headerSize = 14 (file) + 40 (DIB) + 64 (16-entry
  // BGRA palette) = 118 bytes. Pixel data row stride = 144 bytes
  // (288 px / 2, already 4-byte aligned).
  const uint32_t kHeaderSize = 14 + 40 + 64;
  const uint32_t kRowStride  = 144;
  const uint32_t kPixelSize  = kRowStride * 144;
  const uint32_t kTotal      = kHeaderSize + kPixelSize;
  if (outCap < kTotal) return 0;

  // Copy the full source header (file + DIB + palette) so palette
  // entries are preserved. We override the dimensions / sizes / offset
  // fields for our output shape.
  // Source header may be larger than 118 (extended DIB headers, gaps
  // before pixel data); we only copy up to kHeaderSize because that's
  // all the receiver expects in the buildBmp4bpp wire shape.
  const size_t hdrCopy = (srcPxOff < kHeaderSize) ? srcPxOff : kHeaderSize;
  memcpy(out, src, hdrCopy);
  if (hdrCopy < kHeaderSize) {
    // Shouldn't happen given the >=118 check above, but zero the rest
    // of the palette region defensively.
    memset(out + hdrCopy, 0, kHeaderSize - hdrCopy);
  }

  auto wr32 = [](uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);          p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);  p[3] = (uint8_t)((v >> 24) & 0xff);
  };
  out[0] = 'B'; out[1] = 'M';
  wr32(out + 2,  kTotal);                          // bfSize
  wr32(out + 10, kHeaderSize);                     // bfOffBits
  wr32(out + 14, 40);                              // biSize
  wr32(out + 18, (uint32_t)288);                   // biWidth
  wr32(out + 22, (uint32_t)(int32_t)(-144));       // biHeight (top-down)
  out[26] = 1; out[27] = 0;                        // biPlanes
  out[28] = 4; out[29] = 0;                        // biBitCount = 4
  wr32(out + 30, 0);                               // biCompression
  wr32(out + 34, kPixelSize);                      // biSizeImage
  wr32(out + 46, 16);                              // biClrUsed
  wr32(out + 50, 0);                               // biClrImportant

  // ── Pixel-data upscale ──
  // Source byte layout: 144 bytes per row × 144 rows = 20736 B.
  // Quadrant origin in source-image coordinates:
  const size_t srcByteCol0 = (quadrant & 1) ? 72 : 0;   // right vs left half
  const size_t srcRowImg0  = (quadrant & 2) ? 72 : 0;   // bottom vs top half
  const uint8_t* srcPixels = src + srcPxOff;
  uint8_t*       dstPixels = out + kHeaderSize;

  for (size_t outRow = 0; outRow < 144; outRow++) {
    // Each output row pulls from one source row (every two output
    // rows share the same source row — vertical 2× duplication).
    const size_t srcRowImg = srcRowImg0 + (outRow >> 1);
    // If source is bottom-up, row 0 in image-space is at the BOTTOM
    // of the file's pixel data; flip the row index accordingly.
    const size_t srcRowFile = srcTopDown ? srcRowImg : (143 - srcRowImg);
    const uint8_t* srcRow   = srcPixels + srcRowFile * 144 + srcByteCol0;
    uint8_t*       dstRow   = dstPixels + outRow * kRowStride;

    // 72 source bytes (144 source pixels) → 144 output bytes (288
    // output pixels). Each source nibble appears twice in the output.
    for (size_t i = 0; i < 72; i++) {
      const uint8_t b  = srcRow[i];
      const uint8_t hi = (uint8_t)((b >> 4) & 0x0F);
      const uint8_t lo = (uint8_t)( b       & 0x0F);
      dstRow[i * 2]     = (uint8_t)((hi << 4) | hi);
      dstRow[i * 2 + 1] = (uint8_t)((lo << 4) | lo);
    }
  }

  return kTotal;
}

struct BmpFullViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2BmpFullViewerWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;  // identity + NOTIF_SOURCE_G2 for VFS reads
  auto* a = (BmpFullViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAiArm("bmpFullView");
    if (!arm) {
      DEBUG_G2F("[G2] BMP full-viewer: no eligible arm");
      break;
    }
    G2ImageCreateFence cleanupFence = {};

    const char* loadErr = "";
    uint8_t* src = nullptr;
    size_t srcLen = 0;
    int32_t srcW = 0, srcH = 0;
    if (!readBmpFromVfs(String(a->path), &src, &srcLen, &srcW, &srcH, &loadErr)) {
      DEBUG_G2F("[G2] BMP full-viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }
    DEBUG_G2F("[G2] BMP full-viewer: '%s' loaded (%u B, %dx%d)",
              a->path, (unsigned)srcLen, (int)srcW, (int)srcH);

    // Tile geometry — full canvas as 2×2 grid of 288×144 quadrants.
    const int32_t kTileW = 288;
    const int32_t kTileH = 144;
    const G2ImageTile kTiles[4] = {
      { /*x*/ 0,                  /*y*/ 0,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 2, /*name*/ "tileTL" },
      { /*x*/ (uint32_t)kTileW,   /*y*/ 0,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 3, /*name*/ "tileTR" },
      { /*x*/ 0,                  /*y*/ (uint32_t)kTileH,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 4, /*name*/ "tileBL" },
      { /*x*/ (uint32_t)kTileW,   /*y*/ (uint32_t)kTileH,
        /*w*/ (uint32_t)kTileW,   /*h*/ (uint32_t)kTileH,
        /*cid*/ 5, /*name*/ "tileBR" },
    };

    // Each tile's 7 push fragments gets its own block to keep the firmware's
    // per-tile Cmd=4 bookkeeping clear. CREATE correlation rotates separately.
    const uint32_t kPushBase[4] = {
      G2_MAGIC_IMAGE_BASE + 0x11,  // 227..233 TL
      G2_MAGIC_IMAGE_BASE + 0x18,  // 234..240 TR
      G2_MAGIC_IMAGE_BASE + 0x1F,  // 241..247 BL
      G2_MAGIC_IMAGE_BASE + 0x26,  // 248..254 BR
    };

    if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
      DEBUG_G2F("[G2] BMP full-viewer: pre-burst SHUTDOWN failed");
      free(src);
      break;
    }

    // ── Multi-tile CREATE ──
    uint8_t createBuf[256];
    const uint32_t createMagic = allocCreateMagic();
    size_t createLen = g2BuildCreateImageMulti(
        allocSeq(), createMagic, kTiles, /*tileCount*/ 4,
        BLOCKS_WIDGET_ID, createBuf, sizeof(createBuf));
    if (createLen == 0) {
      DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi build failed");
      free(src);
      probePostProbeShutdown(cleanupFence);
      break;
    }
    noteOurShutdownSent();
    if (!probePrepImageCreateAck(createMagic, arm)) {
      free(src);
      break;
    }
    G2ImageCreateFence createFence = {};
    if (!captureImageCreateFence(*arm, &createFence)) {
      g2CreateTxnRelease();
      free(src);
      break;
    }
    if (!g2SendCreateTxnEnvelope(*arm, createBuf, createLen)) {
      DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi TX failed");
      g2CreateTxnRelease();
      free(src);
      probePostProbeShutdown(createFence);
      break;
    }
    if (!probeWaitImageCreateAck(createMagic, kImgCreateAckTimeoutMs,
                                 arm, createFence.generation)) {
      DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi ack timeout — "
                "firmware may not honour the 4-tile geometry");
      free(src);
      probePostProbeShutdown(createFence);
      break;
    }
    DEBUG_G2F("[G2] BMP full-viewer: CREATE-multi acked, pushing 4 tiles");

    // ── Per-tile build + push ──
    const size_t kTileBmpCap = 24 * 1024;
    uint8_t* tileBmp = g2SessionScratchAcquire(kTileBmpCap);
    if (!tileBmp) {
      DEBUG_G2F("[G2] BMP full-viewer: tile scratch alloc failed");
      free(src);
      probePostProbeShutdown(createFence);
      break;
    }

    const char* kTileTag[4] = { "Full/TL", "Full/TR", "Full/BL", "Full/BR" };
    unsigned okTotal = 0, fragTotal = 0;
    bool anyTileFailed = false;
    for (size_t i = 0; i < 4; i++) {
      const size_t tileLen = buildTileBmpFromQuadrant(tileBmp, kTileBmpCap,
                                                       src, srcLen, (uint8_t)i);
      if (tileLen == 0) {
        DEBUG_G2F("[G2] BMP full-viewer: tile %u/4 build failed", (unsigned)(i + 1));
        anyTileFailed = true;
        break;
      }
      unsigned ok = 0, total = 0;
      (void)sendImageBmpFragmentsNoCreate(*arm, kTileTag[i], kPushBase[i],
                                           kTiles[i].containerId,
                                           kTiles[i].containerName,
                                           tileBmp, tileLen, &ok, &total,
                                           /*tolerateMissedAcks*/ 0,
                                           /*inFlightCapOverride*/ 0,
                                           G2_IMG_COMPRESS_RAW, nullptr,
                                           createFence.generation,
                                           /*markExpectedEcho*/ false,
                                           createFence.lifecycleEpoch);
      okTotal   += ok;
      fragTotal += total;
      DEBUG_G2F("[G2] BMP full-viewer: tile %u/4 (%s) shipped %u/%u",
                (unsigned)(i + 1), kTileTag[i], ok, total);
      // Inter-tile gap so the firmware's per-tile reassembly window
      // drains before the next push begins (same pattern as Q12).
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    free(src);

    if (anyTileFailed) {
      probePostProbeShutdown(createFence);
      break;
    }

    DEBUG_G2F("[G2] BMP full-viewer: image up — double-tap to dismiss "
              "(60 s cap, %u/%u total frags)", okTotal, fragTotal);
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] BMP full-viewer: hold ended via %s",
              tapped ? "user tap" : "60 s timeout");

    probePostProbeShutdown(createFence);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;

}

static void g2BmpFullViewerDrop(void* p) {
  auto* a = (BmpFullViewerArgs*)p;
  if (!a) return;
  free(a->path);
  delete a;
}

bool g2ShowBmpFileFullScreen(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  // Heap-low guard — source + tile staging are PSRAM; stack is on g2_session.
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] BMP full-viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new (std::nothrow) BmpFullViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  if (!g2SessionSubmit(g2BmpFullViewerWorker, a, g2BmpFullViewerDrop, "bmp_full",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] BMP full-viewer: session enqueue failed");
    g2BmpFullViewerDrop(a);
    return false;
  }
  return true;
}

// ═════════════════════════════════════════════════════════════════════
// JPG viewer — same wire transport as the BMP viewers above; the only
// difference is a JPEG → 4-bpp BMP conversion step at the front. The
// camera viewer (g2CameraViewerWorker) and camera stream worker both
// take exactly this conversion path on every frame; the JPG file
// viewer reuses the same decoder so any JPEG that the camera could
// produce can also be displayed back to the lens. Required to view
// the camera-stream snapshot files at /sd/PICTURES/cam_<ms>.jpg.
//
// Memory profile (peak):
//   JPEG file copy   ≤ 128 KB    (gated; freed after decode)
//   RGB888 buffer    srcW·srcH·3 (e.g. 240×240 = 173 KB; freed after BMP build)
//   4-bpp BMP        ~21 KB      (held until push completes)
// Image buffers prefer PSRAM; the JPEG HAL owns decoded pixels.
// ═════════════════════════════════════════════════════════════════════

// JPG file viewer — unconditional, using the same portable decoder as live
// capture. No camera sensor dependency is needed for file-based JPEG decode.

// Read JPEG from VFS, decode, and produce a 288×144 4-bpp BMP buffer
// suitable for either single-tile push (g2ShowJpgFile) or 4-tile full-
// screen push (g2ShowJpgFileFullScreen, which then upscales each
// quadrant 2× via buildTileBmpFromQuadrant). On success the caller
// owns *outBmp and must free() it. Errors are returned via outErr
// (static strings — caller doesn't free).
static bool loadJpgAsBmp288x144(const String& rawPath,
                                uint8_t** outBmp, size_t* outBmpLen,
                                const char** outErr) {
  if (outBmp) *outBmp = nullptr;
  if (outBmpLen) *outBmpLen = 0;
  if (outErr) *outErr = "unknown error";

  String path = rawPath;
  path.trim();
  if (path.length() == 0) { if (outErr) *outErr = "empty path"; return false; }
  if (!path.startsWith("/")) path = "/" + path;
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    if (outErr) *outErr = "file not found";
    return false;
  }

  File f = VFS::openGuarded(path, FILE_READ, currentAuthContext());
  if (!f || !f.available()) {
    if (outErr) *outErr = "failed to open file";
    return false;
  }
  const size_t len = (size_t)f.size();
  // 128 KB cap — covers typical phone snapshots and anything the
  // camera-stream worker writes. Larger files almost certainly mean
  // the user dropped a multi-MP JPG on the SD card; the decoded
  // RGB888 would be tens of MB and won't fit even in PSRAM.
  const size_t kMaxJpgBytes = 128 * 1024;
  if (len < 16) {
    f.close();
    if (outErr) *outErr = "file too small for JPEG";
    return false;
  }
  if (len > kMaxJpgBytes) {
    f.close();
    if (outErr) *outErr = "JPEG too large (>128 KB)";
    return false;
  }

  uint8_t* jpg = (uint8_t*)ps_alloc(len, AllocPref::PreferPsram, "g2.jpg.fileLoad");
  if (!jpg) {
    f.close();
    if (outErr) *outErr = "out of memory reading JPG";
    return false;
  }
  const size_t rd = f.read(jpg, len);
  f.close();
  if (rd != len) {
    free(jpg);
    if (outErr) *outErr = "short read from JPG file";
    return false;
  }

  if (!(jpg[0] == 0xFF && jpg[1] == 0xD8)) {
    free(jpg);
    if (outErr) *outErr = "not a JPEG (missing SOI marker)";
    return false;
  }

  hwjpeg::Image decoded;
  const char* decodeError = nullptr;
  if (!decodeJpegForG2(jpg, len, decoded, &decodeError)) {
    free(jpg);
    if (outErr) *outErr = decodeError;
    return false;
  }
  const int32_t srcW = decoded.width;
  const int32_t srcH = decoded.height;
  free(jpg);
  jpg = nullptr;

  // Build 288×144 4-bpp BMP — same target shape readBmpFromVfs
  // requires for the BMP viewer / full-viewer transports. Header
  // (118 B) + 144 × 144 px-bytes ≈ 21 KB.
  const size_t kBmpCap = 14 + 40 + 64 + (288 / 2) * 144 + 64;
  uint8_t* bmp = (uint8_t*)ps_alloc(kBmpCap, AllocPref::PreferPsram, "g2.jpg.bmp");
  if (!bmp) {
    if (outErr) *outErr = "out of memory for BMP build";
    return false;
  }
  const size_t bmpLen = buildBmp4bppFromRgb888(bmp, kBmpCap, decoded.pixels, srcW, srcH);
  decoded.reset();
  if (bmpLen == 0) {
    free(bmp);
    if (outErr) *outErr = "BMP build from RGB888 failed";
    return false;
  }

  if (outBmp) *outBmp = bmp;
  if (outBmpLen) *outBmpLen = bmpLen;
  if (outErr) *outErr = "";
  DEBUG_G2F("[G2] JPG viewer: loaded '%s' src=%dx%d → BMP=%u B",
            path.c_str(), (int)srcW, (int)srcH, (unsigned)bmpLen);
  return true;
}

// Single-tile JPG viewer — mirror of g2BmpViewerWorker. Decodes JPG
// into a 288×144 BMP (top-left of the lens canvas) and pushes via
// sendImageBmpMultiFragment. Magic range borrowed from the BMP
// viewer (0x20/0x21+) — the FSM probe gate prevents both viewers
// from running concurrently, so the magic overlap is benign.
struct JpgViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2JpgViewerWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;  // identity + NOTIF_SOURCE_G2 for VFS reads
  auto* a = (JpgViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAiArm("jpgView");
    if (!arm) {
      DEBUG_G2F("[G2] JPG viewer: no eligible arm");
      break;
    }
    G2ImageCreateFence cleanupFence = {};

    uint8_t* bmp = nullptr;
    size_t bmpLen = 0;
    const char* loadErr = "";
    if (!loadJpgAsBmp288x144(String(a->path), &bmp, &bmpLen, &loadErr)) {
      DEBUG_G2F("[G2] JPG viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }

    const unsigned estFrags =
        (unsigned)((bmpLen + G2_IMG_MAPRAW_CHUNK_BYTES - 1) / G2_IMG_MAPRAW_CHUNK_BYTES);
    const uint32_t kCreateMagic   = allocCreateMagic();
    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+
    if (kPushMagicBase + estFrags >= 256) {
      DEBUG_G2F("[G2] JPG viewer: too many fragments (%u) for magic window",
                estFrags);
      free(bmp);
      break;
    }

    if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
      DEBUG_G2F("[G2] JPG viewer: pre-push SHUTDOWN failed");
      free(bmp);
      break;
    }

    unsigned okFrags = 0, totalFrags = 0;
    (void)sendImageBmpMultiFragment(*arm, "jpgView",
                                    kCreateMagic, kPushMagicBase,
                                    bmp, bmpLen, &okFrags, &totalFrags);
    free(bmp);

    DEBUG_G2F("[G2] JPG viewer: image up — double-tap to dismiss (60 s cap)");
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] JPG viewer: hold ended via %s (frags %u/%u)",
              tapped ? "user tap" : "60 s timeout", okFrags, totalFrags);

    probePostProbeShutdown(cleanupFence);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;

}

static void g2JpgViewerDrop(void* p) {
  auto* a = (JpgViewerArgs*)p;
  if (!a) return;
  free(a->path);
  delete a;
}

bool g2ShowJpgFile(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] JPG viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new (std::nothrow) JpgViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  if (!g2SessionSubmit(g2JpgViewerWorker, a, g2JpgViewerDrop, "jpg_view",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] JPG viewer: session enqueue failed");
    g2JpgViewerDrop(a);
    return false;
  }
  return true;
}

// Full-screen JPG viewer — mirror of g2BmpFullViewerWorker. Reuses the
// 288×144 BMP load above, then tiles to 4 quadrants via the existing
// buildTileBmpFromQuadrant helper. Same wire shape as the BMP
// full-viewer; only the load path differs.
struct JpgFullViewerArgs {
  char* path;
  void (*onDone)();
};

static void g2JpgFullViewerWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;  // identity + NOTIF_SOURCE_G2 for VFS reads
  auto* a = (JpgFullViewerArgs*)arg;

  do {
    G2Temple* arm = pickEvenAiArm("jpgFullView");
    if (!arm) {
      DEBUG_G2F("[G2] JPG full-viewer: no eligible arm");
      break;
    }
    G2ImageCreateFence cleanupFence = {};

    uint8_t* src = nullptr;
    size_t srcLen = 0;
    const char* loadErr = "";
    if (!loadJpgAsBmp288x144(String(a->path), &src, &srcLen, &loadErr)) {
      DEBUG_G2F("[G2] JPG full-viewer: load failed for '%s': %s",
                a->path, loadErr ? loadErr : "?");
      break;
    }

    // Per-tile staging via session scratch (same budget as BMP full-viewer).
    const size_t kTileCap = 14 + 40 + 64 + 144 * 144 + 64;
    uint8_t* tile = g2SessionScratchAcquire(kTileCap);
    if (!tile) {
      DEBUG_G2F("[G2] JPG full-viewer: tile scratch alloc failed (%u B)",
                (unsigned)kTileCap);
      free(src);
      break;
    }

    if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
      DEBUG_G2F("[G2] JPG full-viewer: pre-push SHUTDOWN failed");
      free(src);
      break;
    }

    // Push each quadrant via the same multi-fragment helper the single-tile
    // viewer uses. CREATE correlation rotates for every quadrant transaction;
    // Cmd=3 keeps a separate fixed band for each tile.
    static const uint32_t kPushMagicBases[4] = {
      G2_MAGIC_IMAGE_BASE + 0x31, G2_MAGIC_IMAGE_BASE + 0x41,
      G2_MAGIC_IMAGE_BASE + 0x51, G2_MAGIC_IMAGE_BASE + 0x61,
    };
    unsigned totalOk = 0, totalSent = 0;
    for (uint8_t q = 0; q < 4; q++) {
      const size_t tileLen = buildTileBmpFromQuadrant(tile, kTileCap,
                                                       src, srcLen, q);
      if (tileLen == 0) {
        DEBUG_G2F("[G2] JPG full-viewer: tile %u build failed", (unsigned)q);
        continue;
      }
      unsigned okFrags = 0, totalFrags = 0;
      (void)sendImageBmpMultiFragment(*arm, "jpgFullView",
                                      allocCreateMagic(), kPushMagicBases[q],
                                      tile, tileLen,
                                      &okFrags, &totalFrags);
      totalOk   += okFrags;
      totalSent += totalFrags;
      // Brief inter-tile breather so the firmware can settle the
      // CREATE before we Shutdown+CREATE the next one. Same cadence
      // as the BMP full-viewer.
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    free(src);

    DEBUG_G2F("[G2] JPG full-viewer: image up (%u/%u frags total) — "
              "double-tap to dismiss (60 s cap)", totalOk, totalSent);
    const bool tapped = probeHoldUntilTapOrTimeout(60000);
    DEBUG_G2F("[G2] JPG full-viewer: hold ended via %s",
              tapped ? "user tap" : "60 s timeout");

    probePostProbeShutdown(cleanupFence);
  } while (0);

  if (a->onDone) a->onDone();
  free(a->path);
  delete a;

}

static void g2JpgFullViewerDrop(void* p) {
  auto* a = (JpgFullViewerArgs*)p;
  if (!a) return;
  free(a->path);
  delete a;
}

bool g2ShowJpgFileFullScreen(const char* path, void (*onDone)()) {
  if (!path || !*path) return false;
  if (ESP.getFreeHeap() < 16 * 1024) {
    DEBUG_G2F("[G2] JPG full-viewer: declining — heap low (%u B free)",
              (unsigned)ESP.getFreeHeap());
    return false;
  }
  auto* a = new (std::nothrow) JpgFullViewerArgs;
  if (!a) return false;
  a->path   = strdup(path);
  a->onDone = onDone;
  if (!a->path) {
    delete a;
    return false;
  }
  if (!g2SessionSubmit(g2JpgFullViewerWorker, a, g2JpgFullViewerDrop, "jpg_full",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] JPG full-viewer: session enqueue failed");
    g2JpgFullViewerDrop(a);
    return false;
  }
  return true;
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
