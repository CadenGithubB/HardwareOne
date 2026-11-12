// =============================================================================
// G2 glasses: the Health page (ring vitals)
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"
#include "System_SensorLogging.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "System_Utils.h"
#include "System_MemUtil.h"
#include "G2_HijackCmd.h"

#include "G2_Ring.h"
#include "G2_Health.h"
#include "G2_HijackFsm.h"
#include "G2_GlassesInternal.h"

#if ENABLE_R1_HEALTH
// ─────────────────────────────────────────────────────────────────────
// Health page (g2HealthPageWorker) — mode-switch compound:
//   Overview → list + native text vitals
//   HR/HRV/SpO2/Battery → list + text (title/value) + graph-only image
//   (Q30-proven 3-pane geometry). Shape change = SHUTDOWN + CREATE.
// ─────────────────────────────────────────────────────────────────────
struct HealthPageArgs {
  void (*onDone)();
  uint32_t gen;
};

static void onHealthLoggingCmdDone(bool ok, const char* result,
                                    const G2CmdCookie&, void*) {
  DEBUG_G2_PAGESF("[HEALTH] Health Logging ok=%d → %s", (int)ok,
                 result ? result : "(null)");
  // The live Health worker owns its lens updates and observes logger state
  // on its next loop. Command completions never render on the executor task;
  // the model only records the outcome for that worker to show.
  g2HealthNoteLoggingCommandResult(ok, result);
}

static void g2HealthPageWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;
  auto* a = (HealthPageArgs*)arg;
  const uint32_t myGen = a->gen;

  const G2ContainerGeom kListGeom = G2_GEOM_SPLIT_LIST;
  // Overview: full right column for vitals.
  const G2ContainerGeom kOverviewTextGeom = G2_GEOM_SPLIT_RIGHT;
  // Metric: tall text above graph-only image.
  const G2ContainerGeom kMetricTextGeom = G2_GEOM_SPLIT_RIGHT_TOP;
  static const char* const kTxtName = "txtHealth";
  static constexpr uint32_t kTxtCid = 2;

  G2ImageTile imgTile = {};
  imgTile.x = 280;
  imgTile.y = 128;
  imgTile.w = HEALTH_TILE_W;
  imgTile.h = HEALTH_TILE_H;
  imgTile.containerId   = 3;  // distinct from text cid=2 (3-pane)
  imgTile.containerName = "imgHealth";

  // Push magics must fit in uint8 (≤255). CREATE uses the shared rotating
  // low-byte allocator; the Cmd=4 push band remains the proven 243+ range.
  const uint32_t kPushMagicBase = G2_MAGIC_IMAGE_BASE + 0x21;  // 243
  G2ImageCreateFence metricFence = {};
  G2ImageCreateFence cleanupFence = {};

  auto createHealthOverview = [&](G2Temple& arm, const char* readout) -> bool {
    const uint32_t createLifecycleEpoch =
        __atomic_load_n(&gWidgetLifecycleEpoch, __ATOMIC_ACQUIRE);
    size_t rowCount = 0;
    const char* const* rows = g2HealthMenuRows(&rowCount);
    G2TextChildSpec text = {};
    text.containerName = kTxtName;
    text.content       = readout ? readout : "";
    text.containerId   = kTxtCid;
    text.geom          = kOverviewTextGeom;
    text.eventCapture  = false;
    if (!armCreateSlot("CREATE-health-ov", arm)) return false;
    const uint32_t createMagic = gExpectMagic;
    uint8_t* pb = (uint8_t*)ps_alloc(1024, AllocPref::PreferPsram, "g2.pb.health-ov");
    if (!pb) {
      g2CreateTxnRelease();
      return false;
    }
    size_t pbLen = g2BuildCreateMixedListTextPb(
        createMagic, "lstHealth", rows, rowCount, kListGeom, text,
        BLOCKS_WIDGET_ID, pb, 1024);
    bool sent = pbLen && g2SendCreateTxnPb(arm, pb, pbLen);
    free(pb);
    if (!sent) {
      g2CreateTxnRelease();
      return false;
    }
    if (!waitCreateAck("CREATE-health-ov", BLOCKS_WIDGET_ID)) return false;
    // Load-bearing: tearDownActiveContainer skips SHUTDOWN when
    // containerReady is false. Without this, Overview→metric CREATE
    // lands on a still-live list+text and times out / kicks to Apps.
    if (!g2NoteCreateSuccess(arm, /*isList*/ false, BLOCKS_WIDGET_ID,
                             createLifecycleEpoch)) {
      return false;
    }
    metricFence = {};
    return true;
  };

  auto createHealthMetric = [&](G2Temple& arm, const char* readout) -> bool {
    // Must use sendPbFragmented — a single-fragment envelope over ~240 B
    // is rejected by the glasses firmware with an 8-byte error
    // (seq match, len=0, flag=0x02) and CreateResp never arrives.
    // Two-col metric text + "Tap Poll Now" routinely pushes the CREATE
    // past that ceiling (observed 248 B → timeout; 231 B Overview-era
    // CREATE worked). Overview already fragments; metric must too.
    size_t rowCount = 0;
    const char* const* rows = g2HealthMenuRows(&rowCount);
    G2TextChildSpec text = {};
    text.containerName = kTxtName;
    text.content       = readout ? readout : "";
    text.containerId   = kTxtCid;
    text.geom          = kMetricTextGeom;
    text.eventCapture  = false;
    uint8_t* pb = (uint8_t*)ps_alloc(1400, AllocPref::PreferPsram, "g2.pb.health-m");
    if (!pb) {
      BROADCAST_PRINTF("[G2] health page: CREATE-3pane alloc failed");
      return false;
    }
    const uint32_t createMagic = allocCreateMagic();
    const size_t pbLen = g2BuildCreateMixedListTextImagePb(
        createMagic, /*listName*/ "lstHealth",
        rows, rowCount, kListGeom, text, imgTile, BLOCKS_WIDGET_ID,
        G2_LTI_ORDER_LIST_TEXT_IMAGE, pb, 1400);
    if (pbLen == 0) {
      free(pb);
      BROADCAST_PRINTF("[G2] health page: CREATE-3pane build failed");
      return false;
    }
    if (!probePrepImageCreateAck(createMagic, &arm)) {
      free(pb);
      return false;
    }
    G2ImageCreateFence createFence = {};
    if (!captureImageCreateFence(arm, &createFence)) {
      g2CreateTxnRelease();
      free(pb);
      return false;
    }
    const bool sent = g2SendCreateTxnPb(arm, pb, pbLen);
    free(pb);
    if (!sent) {
      BROADCAST_PRINTF("[G2] health page: CREATE-3pane TX failed");
      g2CreateTxnRelease();
      return false;
    }
    if (!probeWaitImageCreateAck(createMagic, kImgCreateAckTimeoutMs,
                                 &arm, createFence.generation,
                                 /*retainForPublication*/ true)) {
      BROADCAST_PRINTF("[G2] health page: CREATE-3pane ack timeout");
      return false;
    }
    if (!g2NoteCreateSuccess(arm, /*isList*/ false, BLOCKS_WIDGET_ID,
                             createFence.lifecycleEpoch)) {
      return false;
    }
    metricFence = createFence;
    cleanupFence = createFence;
    return true;
  };

  do {
    G2Temple* arm = pickEvenAiArm("healthPage");
    if (!arm) { DEBUG_G2F("[G2] health page: no eligible arm"); break; }

    if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
      DEBUG_G2F("[G2] health page: pre-page SHUTDOWN failed");
      break;
    }
    gImgProbeHoldTapPending = false;
    gImgProbeAbort          = false;
    gImgProbeHoldActive     = false;
    gHealthPagePendingTap   = 0;

    g2HealthOpen();
    g2SetHijackPage(G2_HIJACK_PAGE_HEALTH);

    char readout[160];
    char lastReadout[160];
    lastReadout[0] = '\0';
    g2HealthBuildOverviewText(readout, sizeof(readout));

    bool shapeOverview = true;
    if (!createHealthOverview(*arm, readout)) {
      probePostProbeShutdown(cleanupFence);
      break;
    }
    strlcpy(lastReadout, readout, sizeof(lastReadout));
    DEBUG_G2F("[G2] health page: created Overview (list@'lstHealth' + text@'%s')",
              kTxtName);
    // Leave ImageProbing once Overview is live — staying in that state
    // for the whole Health session swallowed SYSTEM_EXIT reason=0
    // (connection lost) as a page-swap echo.
    imageProbeEnd();
    gHealthTextDead = false;

    gHealthPageActive = true;
    g2HealthSetPageActive(true);
    BROADCAST_PRINTF("[G2] Hijack: Health opened (Overview text / metric graphs)");

    const size_t bmpCap = g2HealthBmpCap();
    uint8_t* bmp = g2SessionScratchAcquire(bmpCap);
    if (!bmp) DEBUG_G2F("[G2] health page: BMP scratch alloc failed (%u B)", (unsigned)bmpCap);

    G2RingHealthRefreshHandle healthRefresh{};
    bool healthRefreshPending = false;
    uint32_t menuGen = g2HealthMenuGeneration();

    auto buildHealthText = [&](char* buf, size_t cap) {
      if (g2HealthWantTextOnlyShape()) {
        g2HealthBuildTextOnly(buf, cap);
      } else {
        g2HealthBuildMetricText(buf, cap);
      }
    };

    while (true) {
      if (!g2LensGetState().hijackActive) { DEBUG_G2F("[G2] health page: hijack ended"); break; }
      if (gImgProbeAbort)      { DEBUG_G2F("[G2] health page: abort flag set"); break; }

      // Worker still servicing the page ⇒ not a stuck hijack. Tap-only
      // refresh previously force-exited ~60 s into a passive graph watch
      // (safety-timeout), then dual-Shutdown raced Apps CREATE and hung
      // bleCentralTx. Mirror camera-stream: live page activity feeds the
      // window; a dead worker stops feeding and the watchdog can still fire.
      gHijackStartedMs = millis();

      const uint32_t taps = gHealthPagePendingTap;
      gHealthPagePendingTap = 0;
      if (taps & (1u << 0)) {
        // Trends submenu Back returns to MAIN Overview; MAIN Back exits.
        if (g2HealthInTrendsNav()) {
          g2HealthAction(0);
          DEBUG_G2F("[G2] health page: Trends Back → main");
        } else {
          DEBUG_G2F("[G2] health page: Back → exit");
          break;
        }
      }
      // The model owns both row sets. Keep tap dispatch aligned with the exact
      // rows used for CREATE instead of baking MAIN/TRENDS counts here.
      size_t activeRowCount = 0;
      (void)g2HealthMenuRows(&activeRowCount);
      if (activeRowCount > 32) activeRowCount = 32;  // tap bitmap width
      for (uint32_t r = 1; r < activeRowCount; r++) {
        if (taps & (1u << r)) g2HealthAction(r);
      }

      if (g2HealthConsumeLoggingToggleRequest()) {
        G2CmdCookie cookie{};
        cookie.targetPage = G2_HIJACK_PAGE_HEALTH;
        cookie.targetNetSub = static_cast<uint8_t>(g2HealthNav());
        // The model mirrors the command's fate so the lens can show it: a
        // neutral label while it is queued, the refusal or error afterwards.
        g2HealthNoteLoggingCommandQueued();
        if (!g2SubmitHijackCommand("healthlogging toggle", cookie,
                                  onHealthLoggingCmdDone, nullptr)) {
          g2HealthNoteLoggingCommandRefused();
          DEBUG_G2_PAGESF("[HEALTH] Health Logging command not queued");
        }
      }

      if (g2HealthConsumePollRequest()) {
        G2RingHealthRefreshHandle requested{};
        if (g2RingStartHealthPageRefresh(requested)) {
          healthRefresh = requested;
          healthRefreshPending = true;
          g2HealthNotePollStarted();
          lastReadout[0] = '\0';  // force UPDATE_TEXT → "Refreshing..."
          DEBUG_G2F("[G2] health page: 2.2.9 DAILY refresh start id=%lu gen=%lu",
                    (unsigned long)requested.id,
                    (unsigned long)requested.generation);
        } else if (!g2RingIsConnected()) {
          DEBUG_G2F("[G2] health page: Poll Now ignored (ring offline)");
        } else {
          g2HealthNotePollUnsupported();
          lastReadout[0] = '\0';
          DEBUG_G2F(
              "[G2] health page: DAILY Poll Now unsupported on active profile; no command queued");
        }
      }

      g2HealthTick();

      bool forceHistory = false;
      if (g2HealthConsumeHistoryRefreshRequest(&forceHistory)) {
        if (!g2RingRequestHistoryRefresh(forceHistory)) {
          DEBUG_G2F("[G2] health page: history %s refresh not queued",
                    forceHistory ? "force" : "normal");
        }
      }
      if (healthRefreshPending) {
        G2RingHealthRefreshStatus status{};
        if (!g2RingGetHealthPageRefreshStatus(healthRefresh, status)) {
          healthRefreshPending = false;
          g2HealthNotePollFailed(/*partial=*/false);
          lastReadout[0] = '\0';
          DEBUG_G2F("[G2] health page: DAILY refresh status invalid");
        } else if (status.completed) {
          healthRefreshPending = false;
          if (status.successful || status.partial) {
            healthLoggingNotePageRefresh();
          }
          if (status.successful) {
            g2HealthNotePollFinished();
          } else {
            g2HealthNotePollFailed(status.partial);
          }
          lastReadout[0] = '\0';
          DEBUG_G2F(
                    "[G2] health page: DAILY refresh done ok=%d partial=%d error=%s",
                    (int)status.successful, (int)status.partial,
                    g2RingTransactionErrorName(status.errorCode));
        }
      }

      // Menu set change (MAIN ↔ Trends) or Overview ↔ metric shape →
      // SHUTDOWN + CREATE. Re-enter ImageProbing for the swap so
      // DISPLAY_OFF / SYSTEM_EXIT reason=1 echoes are suppressed, then
      // leave ImageProbing once the new shape (and any graph push) is live.
      const uint32_t curMenuGen = g2HealthMenuGeneration();
      const bool wantOverview = g2HealthWantTextOnlyShape();
      const bool menuChanged = (curMenuGen != menuGen);
      if (menuChanged || wantOverview != shapeOverview) {
        if (!probeTearDownActiveContainer(*arm, &cleanupFence)) {
          BROADCAST_PRINTF("[G2] health page: shape SHUTDOWN failed");
          // Keep selection but don't exit — retry next loop.
        } else if (wantOverview) {
          buildHealthText(readout, sizeof(readout));
          if (!createHealthOverview(*arm, readout)) {
            BROADCAST_PRINTF("[G2] health page: Overview re-CREATE failed — retrying");
          } else {
            strlcpy(lastReadout, readout, sizeof(lastReadout));
            shapeOverview = true;
            menuGen = curMenuGen;
            g2HealthClearGraphPush();
            gHealthTextDead = false;
            imageProbeEnd();
            DEBUG_G2F("[G2] health page: shape → text-only (nav=%u gen=%u)",
                      (unsigned)g2HealthNav(), (unsigned)menuGen);
          }
        } else if (!bmp) {
          BROADCAST_PRINTF("[G2] health page: no BMP buffer for metric shape");
          imageProbeEnd();
        } else {
          buildHealthText(readout, sizeof(readout));
          if (!createHealthMetric(*arm, readout)) {
            // Recover Overview so the user isn't left on a blank lens.
            BROADCAST_PRINTF("[G2] health page: metric CREATE failed — restoring Overview");
            if (g2HealthInTrendsNav()) g2HealthAction(0);
            else                      g2HealthAction(1);  // MAIN Overview
            buildHealthText(readout, sizeof(readout));
            if (createHealthOverview(*arm, readout)) {
              strlcpy(lastReadout, readout, sizeof(lastReadout));
              shapeOverview = true;
              menuGen = g2HealthMenuGeneration();
              gHealthTextDead = false;
            }
            imageProbeEnd();
          } else {
            strlcpy(lastReadout, readout, sizeof(lastReadout));
            shapeOverview = false;
            menuGen = curMenuGen;
            gHealthTextDead = false;
            // Action already armed a delayed one-shot push; ensure one
            // is scheduled if we got here without a tap arm.
            g2HealthArmGraphPush(400);
            DEBUG_G2F("[G2] health page: shape → metric/trends (list+text+image)");
            // Stay in ImageProbing until the graph push finishes.
          }
        }
      }

      if (shapeOverview) {
        buildHealthText(readout, sizeof(readout));
        if (!gHealthTextDead && strcmp(readout, lastReadout) != 0) {
          if (sendUpdateTextNamed(*arm, kTxtName, kTxtCid, readout)) {
            strlcpy(lastReadout, readout, sizeof(lastReadout));
            // Live vitals refresh = page is healthy (same idea as camera
            // frame pushes). Tap-only refresh kicked users out ~60 s into
            // a passive graph watch.
            if (g2FsmHijackActive()) gHijackStartedMs = millis();
          }
        }
      } else {
        // Metric / Trends 3-pane: native text updates live; graph is one-shot
        // (metric enter / Poll Now / Trends Refresh) — not every 1 Hz sample.
        buildHealthText(readout, sizeof(readout));
        if (!gHealthTextDead && strcmp(readout, lastReadout) != 0) {
          if (sendUpdateTextNamed(*arm, kTxtName, kTxtCid, readout)) {
            strlcpy(lastReadout, readout, sizeof(lastReadout));
            if (g2FsmHijackActive()) gHijackStartedMs = millis();
          }
        }
        if (bmp && g2HealthConsumeGraphPush()) {
          const size_t bmpLen = g2RenderHealthBmp(bmp, bmpCap);
          if (bmpLen > 0) {
            // Re-enter ImageProbing for the burst if we already left it
            // (Poll Now / Refresh after the first graph).
            if (hijackFsmState() != HijackState::ImageProbing) {
              imageProbeBegin();
            }
            // Single push only. g2-kit documents a sacrificial warmup for
            // "first stream after CREATE is dropped", but on FW 2.2.4.34
            // the first ~20 KB stream already paints — a second identical
            // push doubles radio load (L+R+ring) and trips
            // esp_ble_gattc_write_char rc=-1 mid-burst / connection-lost.
            // If a future FW drops the first frame, Poll Now re-pushes.
            //
            // Auto LZ4 bare-block (Even App / Q16d) paints on this mixed
            // list+text+image child; FRAME_CSIZE previously acked blank.
            unsigned okFrags = 0, totalFrags = 0;
            bool pushed = sendImageBmpFragmentsNoCreate(
                *arm, "healthPage",
                kPushMagicBase,
                imgTile.containerId, imgTile.containerName,
                bmp, bmpLen, &okFrags, &totalFrags,
                kImgAckMissTolerance, /*inFlightCapOverride*/ 0,
                G2_IMG_COMPRESS_RAW, nullptr,
                metricFence.generation,
                /*markExpectedEcho*/ false,
                metricFence.lifecycleEpoch);
            // ESP32 rc=-1 mid-burst: one full-push retry after drain
            // (g2-kit desktop BLE does not hit this path).
            if (!pushed && g2LensGetState().hijackActive && !gImgProbeAbort) {
              DEBUG_G2F("[G2] health page: graph push failed — retry in 400 ms");
              vTaskDelay(pdMS_TO_TICKS(400));
              if (g2LensGetState().hijackActive && !gImgProbeAbort) {
                okFrags = totalFrags = 0;
                pushed = sendImageBmpFragmentsNoCreate(
                    *arm, "healthPage",
                    kPushMagicBase,
                    imgTile.containerId, imgTile.containerName,
                    bmp, bmpLen, &okFrags, &totalFrags,
                    kImgAckMissTolerance, /*inFlightCapOverride*/ 0,
                    G2_IMG_COMPRESS_RAW, nullptr,
                    metricFence.generation,
                    /*markExpectedEcho*/ false,
                    metricFence.lifecycleEpoch);
              }
            }
            // Brief settle before UPDATE_TEXT — large Cmd=3 bursts leave
            // the plugin tender on ESP32 + ring.
            vTaskDelay(pdMS_TO_TICKS(400));
            imageProbeEnd();
            if (pushed && g2FsmHijackActive()) gHijackStartedMs = millis();
            else if (!pushed) {
              DEBUG_G2F("[G2] health page: graph push failed (retry via Poll/Refresh)");
            }
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(200));
    }

    // bmp is session scratch — retained across jobs; do not free.
    gHealthPageActive     = false;
    gHealthPagePendingTap = 0;
    g2HealthSetPageActive(false);
    g2HealthClearGraphPush();
    // Safety-timeout / DISPLAY_OFF already tore hijack down (Idle +
    // Shutdown). Re-running probePostProbeShutdown races a second
    // Shutdown into Apps onDone CREATE and can wedge bleCentralTx.
    if (g2LensGetState().hijackActive) {
      probePostProbeShutdown(cleanupFence);
    } else {
      DEBUG_G2F("[G2] health page: hijack already cleared — skip post-probe Shutdown");
      // Benign if already Idle (HijackExit beat us); cleans ImageProbing
      // if only the abort flag flipped.
      imageProbeEnd();
    }
  } while (0);

  // Skip onDone if a newer Health session already started (timeout race:
  // old worker finishes teardown and would otherwise g2ShowAppsMenu over
  // the fresh Health page ~1 s after open).
  // Also skip when hijack is already gone — force-exit (safety-timeout)
  // left Idle; bouncing to Apps CREATE from there is what phantom-
  // rearmed Hijacked and hung BLE TX after the dual-Shutdown race.
  if (a->onDone && myGen == gHealthPageGen && g2LensGetState().hijackActive) a->onDone();
  else if (a->onDone && myGen != gHealthPageGen) {
    DEBUG_G2F("[G2] health page: stale worker gen=%u (current=%u) — skip onDone",
              (unsigned)myGen, (unsigned)gHealthPageGen);
  } else if (a->onDone && !g2LensGetState().hijackActive) {
    DEBUG_G2F("[G2] health page: hijack inactive — skip Apps rebuild onDone");
  }
  delete a;
}

static void g2HealthPageDrop(void* p) { delete (HealthPageArgs*)p; }

bool g2ShowHealthPage(void (*onDone)()) {
  // Session worker aborts any prior job (incl. Health) before running.
  auto* a = new (std::nothrow) HealthPageArgs;
  if (!a) return false;
  a->onDone = onDone;
  a->gen = ++gHealthPageGen;
  if (!g2SessionSubmit(g2HealthPageWorker, a, g2HealthPageDrop, "health_page",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    DEBUG_G2F("[G2] health page: session enqueue failed");
    g2HealthPageDrop(a);
    return false;
  }
  return true;
}

const char* cmd_g2health(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)argsInput;
  if (!isG2Connected()) return "Error: G2 health: not connected";
  if (!g2ShowHealthPage(nullptr))
    return "Error: G2 health: session busy or enqueue failed — retry";
  return "G2 health: opening — Overview/HR/HRV/SpO2/Battery; Toggle Track starts logging; Back to exit.";
}
#endif  // ENABLE_R1_HEALTH

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
