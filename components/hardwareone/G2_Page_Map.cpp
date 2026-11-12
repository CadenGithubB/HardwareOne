// =============================================================================
// G2 glasses: the Maps page
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "G2_HijackCmd.h"

#if ENABLE_MAPS
#include "System_Maps.h"
#include "i2csensor_pa1010d.h"
#endif
#include "G2_GlassesInternal.h"

#if ENABLE_MAPS
// ─────────────────────────────────────────────────────────────────────
// Map viewer — render the loaded offline map HEADLESSLY at the lens's
// native 288×144 into a byte-per-pixel shade buffer (the shared
// OffscreenMapRenderer draws 0-15 per pixel: brightness = feature
// class), pack shades 1:1 into a 288×144 4-bpp grayscale BMP, and push
// it to one lens over the same image transport as g2bmp/camera. Pure
// downstream consumer. Native-res rendering replaced the old 128×64
// 1-bit page + 2.25× nearest-neighbour upscale, so lines are crisp
// (uniform 1 px) instead of alternately 2-and-3 px wide.
// ─────────────────────────────────────────────────────────────────────

// Pack a 288×144 shade buffer (one byte per pixel, values 0..15,
// row-major) into a 288×144 4-bpp top-down BMP. 288×144 is the
// HW-proven native lens-tile size, and the container bytes are
// IDENTICAL to what the proven camera path (buildBmp4bppFromRgb888)
// emits: same 118-byte header, same 16-shade gray palette, same
// 4-byte-aligned 144-byte row stride, same 4-bpp packing (high nibble
// = even column, low = odd). Only the source of the nibbles changed:
// shade values map straight to palette indices instead of 0x0/0xF bits.
// Thin wrapper: the generic core builder does the work (it predates the
// keyboard pad's need for the same format at the same size).
static size_t buildMapBmp4bpp288x144FromShades(uint8_t* out, size_t outCap,
                                               const uint8_t* shades) {
  return g2BuildBmp4bppFromShades(out, outCap, shades, 288, 144);
}

static constexpr int kG2MapLensW = 288;
static constexpr int kG2MapLensH = 144;
static constexpr size_t kG2MapShadeBytes =
    (size_t)kG2MapLensW * kG2MapLensH;

static size_t g2RenderCurrentMapBmp(uint8_t* bmp, size_t cap,
                                   uint8_t* shades, size_t shadeCap,
                                   uint32_t* outRenderPackMs = nullptr) {
  const uint32_t startedMs = millis();
  if (outRenderPackMs) *outRenderPackMs = 0;
  if (!MapCore::hasValidMap() || !bmp || !shades ||
      shadeCap < kG2MapShadeBytes) {
    return 0;
  }
  const LoadedMap& m = MapCore::getCurrentMap();
  if (!gMapCenterSet) {
    gMapCenterLat = (m.header.minLat + m.header.maxLat) / 2000000.0f;
    gMapCenterLon = (m.header.minLon + m.header.maxLon) / 2000000.0f;
    gMapCenterSet = true;
  }
  OffscreenMapRenderer r(shades, kG2MapLensW, kG2MapLensH,
                         kG2MapLensW, kG2MapLensH, 0);
  r.setSurfaceAreas(true);   // G2 lens: also surface water bodies + coastlines
  r.clear();
  // zoom/rotation/layers come from the shared globals via the params snapshot.
  // The viewport is 2.25× the OLED's 128×64, and renderMap's px scale is
  // zoom-only, so multiply zoom by 2.25 to keep the SAME geographic framing
  // as the OLED at the same gMapZoom — just rendered at native lens density.
  // LOD also sees the scaled zoom, so the denser display legitimately shows
  // more detail (e.g. buildings appear at gMapZoom ~0.9 instead of 2.0).
  MapRenderParams params = mapRenderParamsFromGlobals();
  params.zoom *= (float)kG2MapLensW / 128.0f;
  MapCore::renderMap(&r, gMapCenterLat, gMapCenterLon, params);
  const size_t len = buildMapBmp4bpp288x144FromShades(bmp, cap, shades);
  if (outRenderPackMs) *outRenderPackMs = millis() - startedMs;
  return len;
}

// Upper-right status pane for the 3-child Map compound. Keep this ASCII-only:
// the native G2 font reliably renders [ON]/[OFF], while checkmark/emoji glyphs
// vary by glasses firmware. GPS means the sensor runtime toggle; Fix reports
// the latest thread-safe cache snapshot independently.
static void g2MapBuildStatusText(char* out, size_t cap) {
  if (!out || cap == 0) return;
  const LoadedMap& map = MapCore::getCurrentMap();
  const char* filename = map.valid && map.filename[0] ? map.filename : "No map";
#if ENABLE_GPS_SENSOR
  GpsCache gps = {};
  const bool haveSnapshot = gpsCacheSnapshot(gps);
  const char* gpsState = gGpsRunning ? "[ON]" : "[OFF]";
  char fix[24];
  if (!gGpsRunning) {
    strlcpy(fix, "--", sizeof(fix));
  } else if (haveSnapshot && gps.hasFix) {
    snprintf(fix, sizeof(fix), "YES (%u sat)", (unsigned)gps.satellites);
  } else {
    strlcpy(fix, "SEARCH", sizeof(fix));
  }
  snprintf(out, cap, "Map:\n%.28s\nGPS: %s\nFix: %s", filename, gpsState, fix);
#else
  snprintf(out, cap, "Map:\n%.28s\nGPS: N/A", filename);
#endif
}

enum G2MapMenu : uint8_t {
  G2_MAP_MENU_MAIN = 0,
  G2_MAP_MENU_MOVE,
  G2_MAP_MENU_RESULTS,
};

static bool g2MapNameContains(const char* name, const char* query) {
  if (!name || !query || !query[0]) return false;
  const size_t qLen = strlen(query);
  for (const char* p = name; *p; ++p) {
    if (strncasecmp(p, query, qLen) == 0) return true;
  }
  return false;
}

// Build the search-result rows directly from the loaded map's name table.
// Row zero always returns to the compact map menu. The pointers remain valid
// while the map is loaded; the CREATE builder deep-copies them immediately.
static size_t g2MapBuildSearchRows(const char* query, const char** rows,
                                   size_t rowCap) {
  if (!rows || rowCap == 0) return 0;
  rows[0] = "<- Map";
  size_t n = 1;
  const LoadedMap& map = MapCore::getCurrentMap();
  for (uint16_t i = 0; i < map.nameCount && n < rowCap; ++i) {
    const char* name = MapCore::getName(i);
    if (g2MapNameContains(name, query)) rows[n++] = name;
  }
  if (n == 1 && rowCap > 1) rows[n++] = "No matches";
  return n;
}

// Locate the first geometry carrying an exact name and return one of its
// points. This mirrors the OLED map search's tiled scan, but keeps the scan on
// the map worker (not the BLE notify task). A solid name highlight then makes
// the complete road/route/area visible after centering on this point.
static bool g2MapFindNamedFeature(const char* wanted, float* outLat,
                                  float* outLon) {
  if (!wanted || !wanted[0] || !outLat || !outLon) return false;
  MapCacheGuard mapGuard("G2.mapSearch");
  const LoadedMap& map = MapCore::getCurrentMap();
  if (!map.valid || !map.tileDir) return false;

  for (uint16_t tileIdx = 0; tileIdx < map.tileCount; ++tileIdx) {
    size_t tileDataSize = 0;
    const uint8_t* tileData = MapCore::loadTileData(tileIdx, &tileDataSize);
    if (!tileData || tileDataSize < 2) continue;

    const int tx = tileIdx % map.tileGridSize;
    const int ty = tileIdx / map.tileGridSize;
    const int32_t tileMinLon = map.header.minLon + tx * map.tileW - map.haloW;
    const int32_t tileMinLat = map.header.minLat + ty * map.tileH - map.haloH;
    const int32_t haloLonSpan = map.tileW + 2 * map.haloW;
    const int32_t haloLatSpan = map.tileH + 2 * map.haloH;

    const uint8_t* ptr = tileData;
    const uint8_t* end = tileData + tileDataSize;
    const uint16_t featureCount = ptr[0] | (ptr[1] << 8);
    ptr += 2;
    for (uint16_t f = 0; f < featureCount; ++f) {
      if (ptr + HWMAP_FEATURE_HEADER_SIZE > end) break;
      const uint16_t nameIndex  = ptr[2] | (ptr[3] << 8);
      const uint16_t pointCount = ptr[4] | (ptr[5] << 8);
      const size_t featureBytes = HWMAP_FEATURE_HEADER_SIZE + (size_t)pointCount * 4;
      if (ptr + featureBytes > end) break;
      const char* name = MapCore::getName(nameIndex);
      if (pointCount > 0 && name && strcasecmp(name, wanted) == 0) {
        const uint8_t* point = ptr + HWMAP_FEATURE_HEADER_SIZE;
        const uint16_t qLat = point[0] | (point[1] << 8);
        const uint16_t qLon = point[2] | (point[3] << 8);
        const int32_t lat = tileMinLat +
            (int32_t)((int64_t)qLat * haloLatSpan >> 16);
        const int32_t lon = tileMinLon +
            (int32_t)((int64_t)qLon * haloLonSpan >> 16);
        *outLat = lat / 1000000.0f;
        *outLon = lon / 1000000.0f;
        return true;
      }
      ptr += featureBytes;
    }
  }
  return false;
}

static void g2MapSearchCommit(const char* text) {
  strlcpy(gMapSearchQuery, text ? text : "", sizeof(gMapSearchQuery));
  gMapSearchCommitted.store(true, std::memory_order_release);
  g2MapPageWake();
}

static void g2MapSearchCancel() {
  gMapSearchCancelled.store(true, std::memory_order_release);
  g2MapPageWake();
}

// ─────────────────────────────────────────────────────────────────────
// Interactive Maps page — the Health-style 3-pane compound: controls on the
// LEFT, map/GPS status at upper-right, and a 288×144 map image below it.
// Movement re-pushes only the image (Cmd=3); status changes patch only the text
// child (Cmd=5). The list is rebuilt only for explicit Map/Move/Search shape
// transitions because a live compound list REBUILD drops image siblings.
// ─────────────────────────────────────────────────────────────────────
struct MapPageArgs {
  void (*onDone)();
};

static void g2MapPageWorker(void* arg) {
  // Paired-user identity + G2 notify source so tile-loading FS reads succeed.
  G2HijackCtxGuard ctxGuard;
  auto* a = (MapPageArgs*)arg;

  // Keep the landing menu short. Entering Move or Search deliberately rebuilds
  // the compound; actions within Move only repush the image, preserving the
  // fast path for repeated pan/zoom taps.
  static const char* const kMainRows[] = {
    "<- Back",     // 0
    "Move >",      // 1
    "Search",      // 2
  };
  static const char* const kMoveRows[] = {
    "<- Map",      // 0
    "Zoom In",     // 1
    "Zoom Out",    // 2
    "Reset View",  // 3
    "Recenter",    // 4
    "Pan N",       // 5
    "Pan S",       // 6
    "Pan E",       // 7
    "Pan W",       // 8
  };
  static constexpr size_t kMainRowCount =
      sizeof(kMainRows) / sizeof(kMainRows[0]);
  static constexpr size_t kMoveRowCount =
      sizeof(kMoveRows) / sizeof(kMoveRows[0]);
  static constexpr size_t kMaxSearchRows = 16;
  const char* searchRows[kMaxSearchRows] = {};
  size_t searchRowCount = 0;
  G2MapMenu menu = G2_MAP_MENU_MAIN;
  bool waitingForSearch = false;
  uint8_t* mapShades = nullptr;
  uint32_t shadeAllocationCount = 0;
  uint32_t staleRenderCount = 0;
  uint32_t drainedPanCount = 0;

  do {
    G2Temple* arm = pickEvenAiArm("mapPage");
    if (!arm) { DEBUG_G2F("[G2] map page: no eligible arm"); break; }

    // Ensure a map is loaded; auto-load the first available.
    if (!MapCore::hasValidMap()) {
      char maps[8][96];
      const int n = MapCore::getAvailableMaps(maps, 8);
      if (n <= 0) { DEBUG_G2F("[G2] map page: no maps on device (/maps/*.hwmap)"); break; }
      char p[128];
      snprintf(p, sizeof(p), "/maps/%s", maps[0]);
      if (!MapCore::loadMapFile(p)) { DEBUG_G2F("[G2] map page: load failed for '%s'", p); break; }
    }

    // The 41,472-byte native shade surface lives for the whole map-page
    // session. Retry startup allocation briefly, then fail cleanly rather than
    // re-allocating and fragmenting PSRAM on every frame.
    for (uint32_t attempt = 0; attempt < 3 && !mapShades; ++attempt) {
      mapShades = (uint8_t*)ps_alloc(kG2MapShadeBytes,
                                     AllocPref::PreferPsram, "g2.map.shade");
      if (!mapShades && attempt < 2) {
        vTaskDelay(pdMS_TO_TICKS(50u << attempt));
      }
    }
    if (!mapShades) {
      DEBUG_G2F("[G2] map page: shade surface allocation failed (%u B)",
                (unsigned)kG2MapShadeBytes);
      break;
    }
    shadeAllocationCount = 1;

    // Clear stale image-probe state so a leftover flag can't terminate us
    // (matches the camera-stream setup).
    gImgProbeHoldTapPending = false;
    gImgProbeAbort          = false;
    gImgProbeHoldActive     = false;
    gMapPageActionMask.store(0, std::memory_order_release);
    gMapPagePanX.store(0, std::memory_order_release);
    gMapPagePanY.store(0, std::memory_order_release);
    gMapPageRequestGeneration.store(0, std::memory_order_release);
    gMapPageAcceptedPanCount.store(0, std::memory_order_release);
    gMapPagePanSaturationCount.store(0, std::memory_order_release);
    gMapPageLastInputMs.store(millis(), std::memory_order_release);
    gMapPageInputMenu.store((uint8_t)G2_MAP_MENU_MAIN,
                            std::memory_order_release);
    gMapSearchCommitted.store(false, std::memory_order_release);
    gMapSearchCancelled.store(false, std::memory_order_release);
    (void)ulTaskNotifyTake(pdTRUE, 0);

    // Production list+text(+image) split: controls left, status above the map.
    const G2ContainerGeom kListGeom = G2_GEOM_SPLIT_LIST;
    const G2ContainerGeom kStatusGeom = G2_GEOM_SPLIT_RIGHT_TOP;
    static const char* const kStatusName = "txtMap";
    static constexpr uint32_t kStatusCid = 2;
    G2ImageTile imgTile = {};
    imgTile.x = 280;
    imgTile.y = 128;
    imgTile.w = 288;
    imgTile.h = 144;
    imgTile.containerId   = 3;
    imgTile.containerName = "imgMap";

    char statusText[128];
    char lastStatusText[128] = {};
    G2ImageCreateFence surfaceFence = {};

    const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243+ (6 frags → 243..248)
    auto showMapCompound = [&](const char* const* rows, size_t rowCount) -> bool {
      gMapPageActive.store(false, std::memory_order_release);
      if (!probeTearDownActiveContainer(*arm, &surfaceFence)) {
        DEBUG_G2F("[G2] map page: compound SHUTDOWN failed");
        return false;
      }
      g2MapBuildStatusText(statusText, sizeof(statusText));
      G2TextChildSpec status = {};
      status.containerName = kStatusName;
      status.content       = statusText;
      status.containerId   = kStatusCid;
      status.geom          = kStatusGeom;
      status.eventCapture  = false;

      // Search-result rows can make this CREATE exceed one BLE envelope, so
      // use the same fragmented protobuf path as Health's proven 3-pane page.
      uint8_t* pb = (uint8_t*)ps_alloc(1400, AllocPref::PreferPsram,
                                       "g2.pb.map-3pane");
      if (!pb) {
        DEBUG_G2F("[G2] map page: CREATE-3pane alloc failed");
        return false;
      }
      const uint32_t createMagic = allocCreateMagic();
      const size_t pbLen = g2BuildCreateMixedListTextImagePb(
          createMagic, /*listName*/ "lstMap", rows, rowCount, kListGeom,
          status, imgTile, BLOCKS_WIDGET_ID, G2_LTI_ORDER_LIST_TEXT_IMAGE,
          pb, 1400);
      if (pbLen == 0) {
        free(pb);
        DEBUG_G2F("[G2] map page: CREATE-3pane build failed");
        return false;
      }
      if (!probePrepImageCreateAck(createMagic, arm)) {
        free(pb);
        return false;
      }
      G2ImageCreateFence createFence = {};
      if (!captureImageCreateFence(*arm, &createFence)) {
        g2CreateTxnRelease();
        free(pb);
        return false;
      }
      const bool sent = g2SendCreateTxnPb(*arm, pb, pbLen);
      free(pb);
      if (!sent) {
        DEBUG_G2F("[G2] map page: CREATE-3pane TX failed");
        g2CreateTxnRelease();
        return false;
      }
      if (!probeWaitImageCreateAck(createMagic, kImgCreateAckTimeoutMs,
                                   arm, createFence.generation,
                                   /*retainForPublication*/ true)) {
        DEBUG_G2F("[G2] map page: CREATE-3pane ack timeout");
        probePostProbeShutdown(createFence);
        return false;
      }
      if (!g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID,
                               createFence.lifecycleEpoch)) {
        return false;
      }
      surfaceFence = createFence;
      strlcpy(lastStatusText, statusText, sizeof(lastStatusText));
      DEBUG_G2F("[G2] map page: CREATE-3pane acked "
                "(list@'lstMap' + text@'%s' + image@'imgMap')", kStatusName);
      gMapPageInputMenu.store((uint8_t)menu, std::memory_order_release);
      gMapPageActive.store(true, std::memory_order_release);
      return true;
    };

    if (!showMapCompound(kMainRows, kMainRowCount)) break;

    bool dirty = true;       // render the initial frame
    uint32_t lastStatusCheckMs = 0;
    uint32_t consecutiveFrameFailures = 0;

    while (true) {
      if (!g2LensGetState().hijackActive) { DEBUG_G2F("[G2] map page: hijack ended"); break; }
      if (gImgProbeAbort)      { DEBUG_G2F("[G2] map page: abort flag set"); break; }

      // The text-entry overlay owns the lens while Search is active. Its tiny
      // callbacks only publish completion; this worker performs the name scan
      // and the intentional full list+image rebuild afterwards.
      if (waitingForSearch) {
        if (gMapSearchCommitted.load(std::memory_order_acquire) ||
            gMapSearchCancelled.load(std::memory_order_acquire)) {
          const bool committed =
              gMapSearchCommitted.exchange(false, std::memory_order_acq_rel);
          gMapSearchCancelled.store(false, std::memory_order_release);
          waitingForSearch = false;
          if (committed && gMapSearchQuery[0]) {
            searchRowCount = g2MapBuildSearchRows(
                gMapSearchQuery, searchRows, kMaxSearchRows);
            menu = G2_MAP_MENU_RESULTS;
            if (!showMapCompound(searchRows, searchRowCount)) break;
          } else {
            menu = G2_MAP_MENU_MAIN;
            if (!showMapCompound(kMainRows, kMainRowCount)) break;
          }
          dirty = true;
        } else {
          (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
          continue;
        }
      }

      // Drain every pending pan step in one atomic snapshot. Non-pan rows keep
      // action-mask semantics because they are idempotent menu operations.
      const uint32_t taps =
          gMapPageActionMask.exchange(0, std::memory_order_acq_rel);
      const int32_t panX =
          gMapPagePanX.exchange(0, std::memory_order_acq_rel);
      const int32_t panY =
          gMapPagePanY.exchange(0, std::memory_order_acq_rel);
      drainedPanCount += (uint32_t)(panX < 0 ? -panX : panX) +
                         (uint32_t)(panY < 0 ? -panY : panY);
      if (taps || panX || panY) {
        if (menu == G2_MAP_MENU_MAIN) {
          if (taps & (1u << 0)) {
            DEBUG_G2F("[G2] map page: Back → exit");
            break;
          }
          if (taps & (1u << 1)) {
            menu = G2_MAP_MENU_MOVE;
            if (!showMapCompound(kMoveRows, kMoveRowCount)) break;
            dirty = true;
          }
          if (taps & (1u << 2)) {
            gMapPageActive.store(false, std::memory_order_release);
            gMapPageActionMask.store(0, std::memory_order_release);
            gMapPagePanX.store(0, std::memory_order_release);
            gMapPagePanY.store(0, std::memory_order_release);
            gMapSearchCommitted.store(false, std::memory_order_release);
            gMapSearchCancelled.store(false, std::memory_order_release);
            TextEntryConfig cfg = {};
            cfg.prompt = "Map Search";
            cfg.initial = gMapSearchQuery;
            cfg.maxLen = 32;
            cfg.onCommit = g2MapSearchCommit;
            cfg.onCancel = g2MapSearchCancel;
            if (g2BeginTextEntry(cfg)) {
              waitingForSearch = true;
              continue;
            }
            DEBUG_G2F("[G2] map page: search keyboard failed to open");
            gMapPageActive.store(true, std::memory_order_release);
          }
        } else if (menu == G2_MAP_MENU_RESULTS) {
          if (taps & (1u << 0)) {
            menu = G2_MAP_MENU_MAIN;
            if (!showMapCompound(kMainRows, kMainRowCount)) break;
            dirty = true;
          }
          for (size_t row = 1; row < searchRowCount && row < 32; ++row) {
            if (!(taps & (1u << row))) continue;
            if (strcmp(searchRows[row], "No matches") == 0) break;
            float lat = 0.0f, lon = 0.0f;
            if (g2MapFindNamedFeature(searchRows[row], &lat, &lon)) {
              gMapCenterLat = lat;
              gMapCenterLon = lon;
              gMapCenterSet = true;
              gMapManuallyPanned = true;
              mapHighlightByName(searchRows[row], false, 0);
              dirty = true;
              DEBUG_G2F("[G2] map search: centered on '%s'", searchRows[row]);
            }
            break;
          }
        } else {
          if (taps & (1u << 0)) {
            menu = G2_MAP_MENU_MAIN;
            if (!showMapCompound(kMainRows, kMainRowCount)) break;
            dirty = true;
          }
          if (taps & (1u << 1)) { gMapZoom *= 1.5f; if (gMapZoom > 30.0f) gMapZoom = 30.0f; dirty = true; }
          if (taps & (1u << 2)) { gMapZoom /= 1.5f; if (gMapZoom < 0.20f) gMapZoom = 0.20f; dirty = true; }
          if (taps & (1u << 3)) {
            gMapZoom = 1.0f; gMapRotation = 0.0f;
            gMapManuallyPanned = false; mapHighlightClear(); dirty = true;
          }
          if (taps & (1u << 4)) {
            const LoadedMap& cm = MapCore::getCurrentMap();
            gMapCenterLat = (cm.header.minLat + cm.header.maxLat) / 2000000.0f;
            gMapCenterLon = (cm.header.minLon + cm.header.maxLon) / 2000000.0f;
            gMapCenterSet = true;
            gMapManuallyPanned = false;
            dirty = true;
          }
          // Repeated direction taps keep the list intact and only repush the
          // map image. Apply the net intent once with the exact lens viewport
          // and effective render zoom used by g2RenderCurrentMapBmp.
          if (panX || panY) {
            mapPanViewportSteps(
                panX, panY, kG2MapLensW, kG2MapLensH,
                gMapZoom * ((float)kG2MapLensW / 128.0f));
            dirty = true;
          }
        }
      }

      if (dirty) {
        const size_t bmpCap = 14 + 40 + 64 + (288 / 2) * 144 + 64;
        const uint32_t renderGeneration =
            gMapPageRequestGeneration.load(std::memory_order_acquire);
        uint32_t renderPackMs = 0;
        uint8_t* bmp = g2SessionScratchAcquire(bmpCap);
        const size_t bmpLen =
            bmp ? g2RenderCurrentMapBmp(bmp, bmpCap, mapShades,
                                        kG2MapShadeBytes, &renderPackMs)
                : 0;

        // If input advanced while rasterization/packing held the worker, this
        // frame has never reached transport. Drop it and render the newest
        // accumulated center instead of visibly presenting stale motion.
        if (bmpLen > 0 &&
            gMapPageRequestGeneration.load(std::memory_order_acquire) !=
                renderGeneration) {
          ++staleRenderCount;
          DEBUG_MAPS_PERFF("[G2_MAP_PERF] stale pre-transport gen=%u render_pack=%u ms",
                           (unsigned)renderGeneration,
                           (unsigned)renderPackMs);
          continue;
        }

        bool pushed = false;
        unsigned okFrags = 0;
        unsigned totalFrags = 0;
        size_t wireBytes = 0;
        uint32_t pushMs = 0;
        if (bmpLen > 0) {
          if (gMapPageRequestGeneration.load(std::memory_order_acquire) !=
              renderGeneration) {
            ++staleRenderCount;
            DEBUG_MAPS_PERFF("[G2_MAP_PERF] stale at transport gate gen=%u",
                             (unsigned)renderGeneration);
            continue;
          }
          // Push into the existing image child (no re-CREATE). Strict full ACK
          // and one synchronous burst preserve the existing transport policy.
          const uint32_t pushStartedMs = millis();
          pushed = sendImageBmpFragmentsNoCreate(
              *arm, "mapPage", kPushMagicBase,
              imgTile.containerId, imgTile.containerName,
              bmp, bmpLen, &okFrags, &totalFrags,
              /*tolerateMissedAcks*/ 0,
              /*inFlightCapOverride*/ 0,
              G2_IMG_COMPRESS_RAW, &wireBytes,
              surfaceFence.generation,
              /*markExpectedEcho*/ false,
              surfaceFence.lifecycleEpoch);
          pushMs = millis() - pushStartedMs;
        }

        const bool stillCurrent =
            gMapPageRequestGeneration.load(std::memory_order_acquire) ==
            renderGeneration;
        const uint32_t lastInput =
            gMapPageLastInputMs.load(std::memory_order_acquire);
        const uint32_t requestAgeMs =
            renderGeneration ? (millis() - lastInput) : 0;
        DEBUG_MAPS_PERFF("[G2_MAP_PERF] gen=%u age=%u ms render_pack=%u ms "
                         "wire=%u B frags=%u/%u push_ack=%u ms current=%u ok=%u",
                         (unsigned)renderGeneration, (unsigned)requestAgeMs,
                         (unsigned)renderPackMs, (unsigned)wireBytes,
                         okFrags, totalFrags, (unsigned)pushMs,
                         stillCurrent ? 1u : 0u, pushed ? 1u : 0u);
        DEBUG_G2F("[G2] map page: image push %s (%u/%u frags)",
                  pushed ? "complete" : "failed", okFrags, totalFrags);

        if (pushed) {
          consecutiveFrameFailures = 0;
          dirty = !stillCurrent;
        } else {
          dirty = true;
          ++consecutiveFrameFailures;
          const uint32_t shift =
              (consecutiveFrameFailures > 5) ? 5
                                             : consecutiveFrameFailures - 1;
          uint32_t retryMs = 50u << shift;
          if (retryMs > 1000) retryMs = 1000;
          DEBUG_G2F("[G2] map page: frame retry in %u ms (failure %u)",
                    (unsigned)retryMs,
                    (unsigned)consecutiveFrameFailures);
          (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(retryMs));
          continue;
        }
      }

      // GPS runtime/fix state can change without any list interaction. Patch
      // only the status child, at a human-readable cadence, so the map and the
      // native list retain their current pixels/focus.
      const uint32_t nowMs = millis();
      if ((uint32_t)(nowMs - lastStatusCheckMs) >= 1000) {
        lastStatusCheckMs = nowMs;
        g2MapBuildStatusText(statusText, sizeof(statusText));
        if (strcmp(statusText, lastStatusText) != 0 &&
            sendUpdateTextNamed(*arm, kStatusName, kStatusCid, statusText)) {
          strlcpy(lastStatusText, statusText, sizeof(lastStatusText));
        }
      }

      // Sleep only until the next one-second status refresh. Any accepted map
      // tap notifies this task and removes the former fixed 120 ms input tax.
      const uint32_t elapsedSinceStatus = millis() - lastStatusCheckMs;
      const uint32_t waitMs =
          (elapsedSinceStatus >= 1000) ? 1 : (1000 - elapsedSinceStatus);
      (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
    }

    gMapPageActive.store(false, std::memory_order_release);
    gMapPageActionMask.store(0, std::memory_order_release);
    gMapPagePanX.store(0, std::memory_order_release);
    gMapPagePanY.store(0, std::memory_order_release);
    probePostProbeShutdown(surfaceFence);
  } while (0);

  if (mapShades) {
    free(mapShades);
    mapShades = nullptr;
  }
  DEBUG_MAPS_PERFF("[G2_MAP_PERF] session accepted=%u drained=%u saturated=%u "
                   "stale=%u shade_allocs=%u",
                   (unsigned)gMapPageAcceptedPanCount.load(std::memory_order_relaxed),
                   (unsigned)drainedPanCount,
                   (unsigned)gMapPagePanSaturationCount.load(std::memory_order_relaxed),
                   (unsigned)staleRenderCount,
                   (unsigned)shadeAllocationCount);

  if (a->onDone) a->onDone();
  delete a;

}

static void g2MapPageDrop(void* p) { delete (MapPageArgs*)p; }

// Public entry — enqueue on g2_session (8 KB stack paid at G2 init).
bool g2ShowMapPage(void (*onDone)()) {
  // Tile-read staging still needs contiguous internal DRAM; stack does not
  // (session worker already owns 8 KB). Gate on largest free for the margin only.
  const size_t kNeeded  = 8 * 1024;
  const size_t kLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (kLargest < kNeeded) {
    DEBUG_G2F("[G2] map page: declining — need ~%u KB contiguous internal DRAM, largest free %u KB "
              "(free RAM by disconnecting BLE/G2 or disabling unused features)",
              (unsigned)(kNeeded / 1024), (unsigned)(kLargest / 1024));
    return false;
  }
  auto* a = new (std::nothrow) MapPageArgs;
  if (!a) return false;
  a->onDone = onDone;
  if (!g2SessionSubmit(g2MapPageWorker, a, g2MapPageDrop, "map_page",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] map page: session enqueue failed");
    g2MapPageDrop(a);
    return false;
  }
  return true;
}

const char* cmd_g2map(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!isG2Connected()) return "Error: G2 map: not connected";
  if (!g2ShowMapPage(nullptr)) return "Error: G2 map: not enough free internal RAM for tile staging or session busy — free memory then retry";
  return "G2 map: opening interactive map page — tap a control on the lens; Back to exit.";
}
#endif // ENABLE_MAPS

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
