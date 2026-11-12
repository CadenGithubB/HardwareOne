// =============================================================================
// G2 glasses: the microphone detail page
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "System_Microphone.h"
#include "G2_HijackCmd.h"

#include "G2_Page_Sensors.h"
#include "System_Settings.h"
#include "G2_GlassesInternal.h"

// ─────────────────────────────────────────────────────────────────────
// MIC detail compound page — list+text compound where the list is
// CREATEd once (back row, merged toggle, HW row) and the readout text
// child is updated live via Cmd=5 UPDATE_TEXT each tick. Camera-stream
// pattern applied to text: "static list child + per-tick data push to
// the active child". List-row selection stays put across ticks because
// UPDATE_TEXT doesn't touch the list child at all.
// ─────────────────────────────────────────────────────────────────────

// Layout: list left+top, readout text bottom-right corner. Disjoint x
// ranges so the two never overlap.
static constexpr G2ContainerGeom kMicDetailListGeom    = {   8,   8, 350, 270 };
static constexpr G2ContainerGeom kMicDetailReadoutGeom = { 370, 180, 200, 100 };
static constexpr uint32_t        kMicDetailReadoutCid  = 2;
static const char* const         kMicDetailReadoutName = "micLive";

// Last-rendered readout cache. Skip the UPDATE_TEXT wire send when the
// content is byte-identical to what we last shipped — UPDATE_TEXT is
// cheap but the firmware-side render is not free (~ms of plugin-task
// time), and quiet ticks where nothing changed produce no UX value
// from the repaint.
static char gMicDetailLastReadout[80] = {0};

// The lens applier accepts the original command cookie before this handoff.
// Keep the accepted view's generation until the session worker consumes it:
// a refresh already accepted before navigation must not rebuild a later visit.
static uint32_t gMicDetailRecreateMenuGen = 0;
// SHUTDOWN can fail transiently (TX fence busy, write refused) while the page
// and session are still current. Such a request is handed back to the next
// tick instead of ending the live worker, a bounded number of times.
static uint8_t gMicDetailRecreateRetries = 0;
static constexpr uint8_t kMicDetailRecreateRetryCap = 3;

static void requestMicDetailRecreate() {
  const uint32_t menuGen = g2CurrentMenuGen();
  if (g2GetHijackPage() != G2_HIJACK_PAGE_MIC_DETAIL) return;
  gMicDetailRecreateRetries = 0;  // a fresh request gets a fresh budget
  __atomic_store_n(&gMicDetailRecreateMenuGen, menuGen, __ATOMIC_RELEASE);
  liveTextKickRefresh();
}

// Build the live readout string. Three lines so a sustained look at
// the page tells the user the audio level, the I2S configuration, and
// the recording state at a glance. ~25 chars/line budget at the
// readout's 200 px width × firmware near-monospace font.
static void buildMicReadoutText(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if ENABLE_MICROPHONE
  // Source-honest config: the G2 mic is always 16 kHz/16-bit; only PDM honors
  // micSampleRate. The HAL delivers int16 for every source, so bits is always 16.
  const bool g2 = (audioGetSource() == AUDIO_SRC_G2_LEFT);
  const int hz  = g2 ? 16000 : micSampleRate;
  if (!gMicRunning) {
    // I2S engine off: no level to read. Print the static config so the
    // page doesn't go visually empty when the user toggles off.
    snprintf(out, cap,
             "Lvl: --%%\n%dHz 16b %dch\nidle",
             hz, micChannels);
    return;
  }
  const int level = getAudioLevel();
  snprintf(out, cap,
           "Lvl: %d%%\n%dHz 16b %dch\n%s",
           level, hz, micChannels,
           micRecordingBusy() ? "rec" : "idle");
#else
  snprintf(out, cap, "MIC unavailable");
#endif
}

// Live render fn driven by liveTextWorker. First call (containerReady=
// false): CREATE the compound (1 list + 1 text). Subsequent ticks:
// UPDATE_TEXT on the readout child only — list child untouched, so
// selection stays put. Command completions request a fresh CREATE, but
// teardown and rendering both stay on this session worker so a periodic
// UPDATE_TEXT cannot race the SHUTDOWN/CREATE sequence.
static bool renderMicDetailLive() {
  const uint32_t redrawMenuGen = __atomic_exchange_n(
      &gMicDetailRecreateMenuGen, 0u, __ATOMIC_ACQ_REL);
  if (redrawMenuGen != 0 &&
      (redrawMenuGen != g2CurrentMenuGen() ||
       g2GetHijackPage() != G2_HIJACK_PAGE_MIC_DETAIL)) return false;
  const bool recreate = redrawMenuGen != 0;
  G2Temple* arm = (gR.connected && !gR.pluginDead) ? &gR
                : (gL.connected && !gL.pluginDead) ? &gL
                                                   : nullptr;
  if (!arm) {
    DEBUG_G2F("[G2] mic-detail: no eligible temple");
    return false;
  }
  const uint32_t createLifecycleEpoch =
      __atomic_load_n(&gWidgetLifecycleEpoch, __ATOMIC_ACQUIRE);
  // REBUILD-list would blank the compound's sibling text child. A local
  // clear alone also leaves the firmware's old compound live. Use the
  // existing session/connection-fenced SHUTDOWN and stop if it fails.
  if (recreate) {
    if (!tearDownActiveContainer(*arm)) {
      // A transient SHUTDOWN failure with the page, navigation and session
      // still current is handed back to the next tick (bounded) instead of
      // ending the worker and leaving the page dead. A stale fence or an
      // exhausted budget ends it as before.
      const bool fenceCurrent = redrawMenuGen == g2CurrentMenuGen() &&
                                g2GetHijackPage() == G2_HIJACK_PAGE_MIC_DETAIL &&
                                g2SessionLifecycleAllows(createLifecycleEpoch);
      if (fenceCurrent && gMicDetailRecreateRetries < kMicDetailRecreateRetryCap) {
        ++gMicDetailRecreateRetries;
        uint32_t idle = 0;  // re-publish only if no newer request landed meanwhile
        __atomic_compare_exchange_n(&gMicDetailRecreateMenuGen, &idle, redrawMenuGen,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        DEBUG_G2F("[G2] mic-detail: refresh SHUTDOWN failed; retry %u/%u on the next tick",
                  (unsigned)gMicDetailRecreateRetries, (unsigned)kMicDetailRecreateRetryCap);
        return true;
      }
      DEBUG_G2F("[G2] mic-detail: refresh SHUTDOWN failed");
      return false;
    }
    gMicDetailRecreateRetries = 0;
    // Navigation can change while SHUTDOWN settles. Do not CREATE the old
    // compound over the destination page, even if the request was current
    // when the renderer consumed it.
    if (redrawMenuGen != g2CurrentMenuGen() ||
        g2GetHijackPage() != G2_HIJACK_PAGE_MIC_DETAIL ||
        !g2SessionLifecycleAllows(createLifecycleEpoch)) return false;
  }

#if ENABLE_MICROPHONE
  // Re-assert the glasses' LC3 stream if a container teardown stopped it (see
  // g2MicNoteStreamStoppedByTeardown). This page tears the container down after
  // every action to relabel its list rows, and each teardown silently kills the
  // mic — so without this the level meter and any recording read zeros forever.
  //
  // Done here rather than at the CREATE chokepoint on purpose: liveTextWorker is
  // a plain task context where a BLE send is safe, whereas g2NoteCreateSuccess
  // can run under the notify path where sending risks stalling the stack. Cost
  // of the polling approach is at most one tick (1 s) of audio per redraw.
  // containerReady is load-bearing: the firmware only emits audio while a page
  // is up, so arming during the CREATE tick (when it is still false) would send
  // AudioCtrCmd{en=1} into a torn-down lens, set gMicStreamOn, and then the
  // idempotency check would suppress the re-arm that would actually have stuck.
  // Waiting one tick for the container costs a second and always lands.
  if (!recreate && g2TempleReadyAtGeneration(gL) &&
      g2LensGetState().containerReady &&
      audioGetSource() == AUDIO_SRC_G2_LEFT &&
      audioCaptureActive() && !gMicStreamOn) {
    if (g2MicStreamEnable(true)) {
      DEBUG_G2F("[G2-MIC] stream re-asserted after container re-CREATE");
    }
  }
#endif

  // Build readout text fresh every tick (cheap — getAudioLevel() reads a
  // small sample window, not a full DMA buffer).
  char readout[80];
  buildMicReadoutText(readout, sizeof(readout));

  if (recreate || !g2LensGetState().containerReady) {
    // First call (or post-toggle re-init): CREATE compound. After teardown,
    // do not depend on the asynchronously updated lens mirror catching up.
    // List rows mirror what the legacy generic Sensors-detail showed
    // for MIC, with the merged "MIC: <hw> | <state>" row replacing the
    // old 2-row pair. Drives the same toggle as the legacy row when
    // tapped (idx 1).
    // MIC: On/Off only — PDM has no probeable "connected" signal
    // (no chip ID, no I2C address). See G2_Page_Sensors.cpp's
    // isMic branch for the same rationale.
    char toggleRow[40];
    const bool en = gMicRunning;
    snprintf(toggleRow, sizeof(toggleRow),
             "MIC: %s",
             en ? "On" : "Off");
    // Record row (idx 2) — recording needs the mic on, so it reads "mic off"
    // and no-ops until then; otherwise it is a start/stop toggle keyed on
    // the synchronized recording lifecycle (auto-names rec_<ms>.wav, 60 s
    // cap). List rows can't
    // UPDATE_TEXT, so the tap forces a re-CREATE to flip the label.
    char recRow[40];
    if (!en)                 snprintf(recRow, sizeof(recRow), "Rec: mic off");
    else if (micRecordingBusy()) snprintf(recRow, sizeof(recRow), "Stop recording");
    else                     snprintf(recRow, sizeof(recRow), "Record 60s");
    // Source selector row (idx 3) — tap cycles the preference among "auto" plus
    // whatever sources are actually connected (see g2MicDetailHandleTap). Shows
    // the preference, and the resolved active source when it differs.
    char srcRow[40];
    {
      const char* pref = gSettings.micSource.c_str();
      const char* active = (audioGetSource() == AUDIO_SRC_G2_LEFT)  ? "g2"
                         : (audioGetSource() == AUDIO_SRC_LOCAL_PDM) ? "pdm"
                                                                     : "none";
      if (en && strcmp(pref, active) != 0) {
        snprintf(srcRow, sizeof(srcRow), "Src: %s (%s)", pref, active);
      } else {
        snprintf(srcRow, sizeof(srcRow), "Src: %s", pref);
      }
    }
    const char* listItems[4] = {
      "<- Hardware",
      toggleRow,
      recRow,
      srcRow,
    };
    G2TextChildSpec textChild = {};
    textChild.containerName = kMicDetailReadoutName;
    textChild.containerId   = kMicDetailReadoutCid;
    textChild.content       = readout;
    textChild.geom          = kMicDetailReadoutGeom;
    textChild.eventCapture  = false;
    bool ok = sendCreateMixedListMultiTextAndWait(
        *arm,
        listItems, 4, kMicDetailListGeom,
        &textChild, 1,
        BLOCKS_WIDGET_ID);
    if (!ok) {
      DEBUG_G2F("[G2] mic-detail: CREATE-list+text failed");
      return false;
    }
    // Mirror container state into the FSM. isList=false because this is
    // a mixed-mode compound — same convention renderStatusCompound uses.
    uint32_t presentationEpoch = 0;
    if (!g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID,
                             createLifecycleEpoch, &presentationEpoch)) {
      return false;
    }
    // The generic live worker publishes the initial exit callback. A
    // recreation advances presentation identity, so bind its replacement
    // callback to this exact committed CREATE as well.
    if (recreate && !g2TextViewPublishForLifecycle(
                        liveTextExitToHijackMenu, nullptr,
                        createLifecycleEpoch, presentationEpoch)) {
      return false;
    }
    strlcpy(gMicDetailLastReadout, readout, sizeof(gMicDetailLastReadout));
    DEBUG_G2F("[G2] mic-detail: initial CREATE acked (1 list + 1 text)");
    return true;
  }

  // Subsequent ticks: UPDATE_TEXT on the readout child, gated by
  // byte-identical cache. List child untouched → selection persists.
  if (strcmp(readout, gMicDetailLastReadout) == 0) {
    return true;  // quiet tick, no wire I/O
  }
  bool ok = sendUpdateTextNamed(*arm,
                                kMicDetailReadoutName, kMicDetailReadoutCid,
                                readout);
  if (ok) {
    strlcpy(gMicDetailLastReadout, readout, sizeof(gMicDetailLastReadout));
  }
  return ok;
}

// Stub buildText for the registry's required-field check. Hidden
// pages that drive the lens via liveRender don't have a CLI text
// view, but g2RegisterPage rejects entries with null buildText —
// without this stub the page never makes it into the registry,
// and handleHijackMenuTap's lookup falls through to the read-only
// TEXT_VIEW fallback (idx 0 → MAIN, rest no-op), which is exactly
// the symptom that masked itself as "tap not firing" on
// 2026-05-08. If someone runs `cmd_g2micdetail` from the CLI it
// just emits the static info line; the live data path is on the
// lens via renderMicDetailLive.
static void g2BuildMicDetailInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap,
           "MIC detail (live readout on lens; CLI view not implemented)");
}

const G2PageModule kMicDetailPage = {
  // Hidden — hijackLabel=nullptr keeps it out of the main menu, the
  // only entry is g2ShowMicDetail() called from Sensors-detail.
  "micdetail", nullptr,
  "MIC detail page (hidden — reached via Sensors → MIC)",
  /*buildText=*/  g2BuildMicDetailInfo,   // required by g2RegisterPage; stub
  /*showMenu=*/   nullptr,
  /*handleTap=*/  g2MicDetailHandleTap,
  G2_HIJACK_PAGE_MIC_DETAIL,
  // 1 Hz tick — audio level fluctuates fast enough to feel live at
  // 1 Hz; the rate doesn't strain the BLE link (single-envelope
  // UPDATE_TEXT, no fragmenting). Bump down (200–500 ms) if the user
  // wants more responsive level updates.
  /*liveIntervalMs=*/ 1000,
  /*backLabel=*/      "<- Hardware",
  /*prefersTextWidget=*/ true,    // routes through g2StartLiveTextPage
  /*liveRender=*/     renderMicDetailLive,
};

bool g2ShowMicDetail() {
  __atomic_store_n(&gMicDetailRecreateMenuGen, 0u, __ATOMIC_RELEASE);
  gMicDetailRecreateRetries = 0;
  // Mirror handleHijackMenuTap's live-text dispatch (the menu does this
  // when the user taps "Status" etc.). For MIC detail the entry isn't
  // the main menu — it's Sensors-detail tap on MIC — so we replicate
  // the dispatch inline. liveTextWorker handles SHUTDOWN of the
  // outgoing list page, then calls renderMicDetailLive (which CREATEs
  // the compound on first call).
  if (!g2StartLiveTextPage(/*buildText=*/ nullptr,
                           kMicDetailPage.liveIntervalMs,
                           kMicDetailPage.liveRender)) {
    DEBUG_G2F("[G2] mic-detail: live-text spawn failed");
    return false;
  }
  g2SetHijackPage(G2_HIJACK_PAGE_MIC_DETAIL);
  // Reset the readout cache so the first tick after entry isn't
  // suppressed by a stale value left over from the previous session.
  gMicDetailLastReadout[0] = '\0';
  BROADCAST_PRINTF("[G2] Hijack: MIC detail opened (live readout @1 Hz)");
  return true;
}

#if ENABLE_MICROPHONE
// openmic/closemic done — persist only on success, then re-CREATE so the
// toggle row reads gMicRunning (list rows can't UPDATE_TEXT).
// userData = (uintptr_t)1 if we wanted ON, else 0.
static void onMicDetailRuntimeDone(bool ok, const char* result,
                                   const G2CmdCookie& cookie,
                                   void* userData) {
  const bool wantOn = ((uintptr_t)userData) != 0;
  if (ok) {
    char line[32];
    snprintf(line, sizeof(line), "micautostart %s", wantOn ? "on" : "off");
    if (!g2SubmitHijackCommand(line, cookie, nullptr, nullptr)) {
      DEBUG_G2F("[G2] MIC detail: %s submit FAILED after runtime ok", line);
    }
  } else {
    DEBUG_G2F("[G2] MIC detail: runtime failed — skip micautostart "
              "(wantOn=%d result='%s')",
              wantOn ? 1 : 0, result ? result : "");
  }

  (void)g2EnqueueRedraw(cookie, requestMicDetailRecreate, "MIC runtime");
}

// micrecord start/stop done — re-CREATE so the Record row label flips
// (list rows can't UPDATE_TEXT). No micautostart chaining — recording is a
// one-shot, not a boot preference. The ~1 s worst-case flush on `stop` (WAV
// header) runs on cmd_exec, not here.
static void onMicDetailRecordDone(bool ok, const char* result,
                                  const G2CmdCookie& cookie, void* /*userData*/) {
  if (!ok) {
    DEBUG_G2F("[G2] MIC detail: record cmd result '%s'", result ? result : "");
  }
  (void)g2EnqueueRedraw(cookie, requestMicDetailRecreate, "MIC record");
}
#endif

// Redraw-only callback for the source-selector tap. `micsource` may restart the
// mic (stop→set→start) or just persist the preference; either way the "Src:" row
// label must refresh, and list rows can't UPDATE_TEXT — so force a fresh CREATE
// (the same teardown the toggle uses, without the micautostart write).
static void onMicDetailSourceDone(bool /*ok*/, const char* /*result*/,
                                  const G2CmdCookie& cookie, void* /*userData*/) {
  (void)g2EnqueueRedraw(cookie, requestMicDetailRecreate, "MIC source");
}

void g2MicDetailHandleTap(uint32_t idx) {
  if (idx == 0) {
    // Back to Sensors landing list. The live-text worker will see
    // gLiveTextStopFlag get tripped by the outgoing tear-down and
    // exit cleanly; g2ShowSensorsMenu re-CREATEs the sensor list.
    g2ShowSensorsMenu();
    return;
  }
  if (idx == 1) {
#if ENABLE_MICROPHONE
    // Toggle — open/close via cmd_exec; persist + redraw in callback
    // after gMicRunning has actually changed (never race the CREATE).
    const bool prev = gMicRunning;
    const bool next = !prev;
    BROADCAST_PRINTF("[G2] MIC detail: toggle %s -> %s",
                     prev ? "ON" : "OFF",
                     next ? "ON" : "OFF");
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = 0;
    const char* runtime = next ? "openmic" : "closemic";
    if (!g2SubmitHijackCommand(runtime, cookie, onMicDetailRuntimeDone,
                               (void*)(uintptr_t)(next ? 1 : 0))) {
      DEBUG_G2F("[G2] MIC detail: %s submit FAILED — no inline mutate", runtime);
    }
#endif
    return;
  }
  if (idx == 2) {
#if ENABLE_MICROPHONE
    // Record toggle. Recording requires the mic on (micrecord errors
    // otherwise) — no-op the tap so the row's "mic off" label is honest.
    if (!gMicRunning) {
      DEBUG_G2F("[G2] MIC detail: record tapped but mic off — no-op");
      return;
    }
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = 0;
    const char* line = micRecordingBusy() ? "micrecord stop" : "micrecord start";
    BROADCAST_PRINTF("[G2] MIC detail: %s", line);
    if (!g2SubmitHijackCommand(line, cookie, onMicDetailRecordDone, nullptr)) {
      DEBUG_G2F("[G2] MIC detail: '%s' submit FAILED — no inline mutate", line);
    }
#endif
    return;
  }
  if (idx == 3) {
#if ENABLE_MICROPHONE
    // Source selector — cycle the preference among "auto" plus the sources that
    // are actually connected right now (so a PDM-less board only offers auto/g2).
    // `micsource` applies it live if the mic is running (stop→set→start).
    const char* opts[3];
    int nOpt = 0;
    opts[nOpt++] = "auto";
    if (audioSourceAvailable(AUDIO_SRC_LOCAL_PDM)) opts[nOpt++] = "pdm";
    if (audioSourceAvailable(AUDIO_SRC_G2_LEFT))   opts[nOpt++] = "g2";
    int cur = 0;
    for (int i = 0; i < nOpt; i++) {
      if (gSettings.micSource == opts[i]) { cur = i; break; }
    }
    const char* next = opts[(cur + 1) % nOpt];
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = 0;
    char line[24];
    snprintf(line, sizeof(line), "micsource %s", next);
    BROADCAST_PRINTF("[G2] MIC detail: source -> %s", next);
    if (!g2SubmitHijackCommand(line, cookie, onMicDetailSourceDone, nullptr)) {
      DEBUG_G2F("[G2] MIC detail: 'micsource' submit FAILED — no inline mutate");
    }
#endif
    return;
  }
  DEBUG_G2F("[G2] MIC detail: tap idx=%u (no action)", (unsigned)idx);
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
