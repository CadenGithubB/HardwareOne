// =============================================================================
// G2 glasses: the FM radio tuner page
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "G2_HijackCmd.h"
#include "G2_ReplyNotice.h"

#include "G2_Page_Sensors.h"
#include "i2csensor_rda5807.h"
#include "G2_GlassesInternal.h"

#if ENABLE_FM_RADIO
// ─────────────────────────────────────────────────────────────────────
// FM radio tuner — action rows beside a live radio readout. Reached by
// tapping the FM row on the Sensors page (which intercepts before the
// generic sensor-live detail, LED-precedent). Same live-text compound
// shape as the Ring dashboard / sensor-live pages: a multi-row action
// list on the left + a text child refreshed via UPDATE_TEXT on the
// right, so tapping rows never disturbs the readout and vice versa.
// Row labels are STATIC by design (list rows can't UPDATE_TEXT and a
// REBUILD blanks the sibling text child) — all state (frequency or seek
// progress, RDS, volume/mute, signal) lives in the readout. Actions
// dispatch the real fmradio* commands via cmd_exec, which decides who may
// run them (most need an admin); a refusal shows at the top of the readout.
// The async-seek core means every command returns in milliseconds.
// ─────────────────────────────────────────────────────────────────────
static constexpr G2ContainerGeom kFmListGeom    = {   8,   8, 200, 270 };
static constexpr G2ContainerGeom kFmReadoutGeom = { 216,   8, 352, 270 };
static constexpr uint32_t        kFmReadoutCid  = 2;
static const char* const         kFmReadoutName = "fmLive";
static char gFmLastReadout[160] = {0};

static const char* const kFmTunerRows[] = {
  "<- Hardware", "Seek Up", "Seek Down", "Vol +", "Vol -", "Mute", "Power",
};
static constexpr size_t kFmTunerRowCount =
    sizeof(kFmTunerRows) / sizeof(kFmTunerRows[0]);

static bool fmTunerRadioOn() { return gFmRadioConnected && gRadioInitialized; }

// Build the readout. Radio-off shows a hint instead of stale numbers; the
// cache mutex may not even exist yet in that state (created by the fmRadio
// task), so check the lifecycle globals BEFORE touching the cache.
static void buildFmTunerReadout(char* out, size_t cap) {
  if (!out || cap == 0) return;
  if (!fmTunerRadioOn()) {
    snprintf(out, cap, "Radio off\nTap Power\nto start");
    return;
  }
  FmRadioCache snap{};
  if (!fmRadioCacheSnapshot(snap)) {
    snprintf(out, cap, "Radio busy\n(reading...)");
    return;
  }
  char freq[20];
  if (snap.seekInProgress) {
    snprintf(freq, sizeof(freq), "Seeking %s...", snap.seekDirUp ? "up" : "down");
  } else {
    snprintf(freq, sizeof(freq), "%.1f MHz", snap.frequency / 100.0);
  }
  char vol[20];
  if (snap.muted) {
    snprintf(vol, sizeof(vol), "Muted");
  } else {
    snprintf(vol, sizeof(vol), "Vol:%u %s", (unsigned)snap.volume,
             snap.stereo ? "Stereo" : "Mono");
  }
  snprintf(out, cap, "%s\nRDS: %s\n%s\nRSSI:%u SNR:%u",
           freq,
           snap.stationName[0] ? snap.stationName : "--",
           vol,
           (unsigned)snap.rssi, (unsigned)snap.snr);
}

// The text of the readout child: a refused tap's reason ("Needs an admin") on
// top for a few seconds, then the radio state. Row labels are static, so the
// readout is where the lens answers a tap.
static void buildFmTunerLensText(char* out, size_t cap) {
  if (!out || cap == 0) return;
  char notice[kG2NoticeMax];
  if (!g2LensNoticePeek(G2_HIJACK_PAGE_FMRADIO, notice, sizeof(notice))) {
    buildFmTunerReadout(out, cap);
    return;
  }
  char state[160];
  buildFmTunerReadout(state, sizeof(state));
  snprintf(out, cap, "%s\n%s", notice, state);
}

// Live render fn driven by liveTextWorker. First call: CREATE the compound
// (7-row action list + 1 text child). Subsequent ticks: UPDATE_TEXT on the
// readout child only, gated by a byte-identical cache. Mirrors other live-text pages.
static bool renderFmTunerLive() {
  G2Temple* arm = (gR.connected && !gR.pluginDead) ? &gR
                : (gL.connected && !gL.pluginDead) ? &gL
                                                   : nullptr;
  if (!arm) {
    DEBUG_G2F("[G2] fmtuner: no eligible temple");
    return false;
  }

  char readout[160];
  buildFmTunerLensText(readout, sizeof(readout));

  if (!g2LensGetState().containerReady) {
    const uint32_t createLifecycleEpoch =
        __atomic_load_n(&gWidgetLifecycleEpoch, __ATOMIC_ACQUIRE);
    G2TextChildSpec textChild = {};
    textChild.containerName = kFmReadoutName;
    textChild.containerId   = kFmReadoutCid;
    textChild.content       = readout;
    textChild.geom          = kFmReadoutGeom;
    textChild.eventCapture  = false;
    bool ok = sendCreateMixedListMultiTextAndWait(
        *arm, kFmTunerRows, kFmTunerRowCount, kFmListGeom, &textChild, 1,
        BLOCKS_WIDGET_ID);
    if (!ok) {
      DEBUG_G2F("[G2] fmtuner: CREATE-list+text failed");
      return false;
    }
    if (!g2NoteCreateSuccess(*arm, /*isList*/ false, BLOCKS_WIDGET_ID,
                             createLifecycleEpoch)) {
      return false;
    }
    strlcpy(gFmLastReadout, readout, sizeof(gFmLastReadout));
    return true;
  }

  if (strcmp(readout, gFmLastReadout) == 0) {
    return true;  // quiet tick, no wire I/O
  }
  bool ok = sendUpdateTextNamed(*arm, kFmReadoutName, kFmReadoutCid, readout);
  if (ok) {
    strlcpy(gFmLastReadout, readout, sizeof(gFmLastReadout));
  }
  return ok;
}

// Command completion — just kick an immediate re-render so the readout
// reflects the action within ~100 ms instead of waiting out the 1 s tick
// (the cache is already updated by the time the handler returns; seek shows
// its progress flag the same way). A refusal was already recorded as this
// page's lens notice, which the readout shows. Semaphore give is safe from
// cmd_exec.
static void onFmTunerCmdDone(bool ok, const char* result,
                             const G2CmdCookie& /*cookie*/, void* /*userData*/) {
  DEBUG_G2F("[G2] fmtuner cmd done: ok=%d result='%s'", (int)ok,
            result ? result : "");
  liveTextKickRefresh();
}

static void g2FmTunerSubmit(const char* line) {
  G2CmdCookie cookie{};
  cookie.targetPage = g2GetHijackPage();
  if (!g2SubmitHijackCommand(line, cookie, onFmTunerCmdDone, nullptr)) {
    DEBUG_G2F("[G2] fmtuner: submit FAILED for '%s'", line);
  }
}

static void g2FmTunerHandleTap(uint32_t idx) {
  if (idx == 0) {   // <- Sensors
    g2ShowSensorsMenu();
    return;
  }
  const bool on = fmTunerRadioOn();
  if (idx == 6) {   // Power toggle — the only action valid while off
    g2FmTunerSubmit(on ? "closefmradio" : "openfmradio");
    return;
  }
  if (!on) return;  // seek/vol/mute are no-ops until the radio runs

  FmRadioCache radio{};
  if (!fmRadioCacheSnapshot(radio)) return;

  switch (idx) {
    case 1:   // Seek Up
    case 2: { // Seek Down
      if (radio.seekInProgress) return;  // one seek at a time
      g2FmTunerSubmit(idx == 1 ? "fmradioseek up" : "fmradioseek down");
      return;
    }
    case 3:   // Vol +
    case 4: { // Vol -
      // ±3 steps keep tap counts sane
      // across the 0-15 range (mirrors the OLED's preset-ladder spirit).
      int v = (int)radio.volume + (idx == 3 ? 3 : -3);
      if (v < 0) v = 0;
      if (v > 15) v = 15;
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "fmradiovolume %d", v);
      g2FmTunerSubmit(cmd);
      return;
    }
    case 5:   // Mute toggle — static label, conditional dispatch
      g2FmTunerSubmit(radio.muted ? "fmradiomute off" : "fmradiomute on");
      return;
    default:
      return;
  }
}

// Stub buildText for the registry's required-field check (hidden live pages
// have no CLI text view; `fmradioread` is the CLI form).
static void g2BuildFmTunerInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "FM tuner (live controls on lens; 'fmradioread' for CLI)");
}

const G2PageModule kFmTunerPage = {
  // Hidden — hijackLabel=nullptr keeps it out of the main menu; the only
  // entry is g2ShowFmTunerPage() from the Sensors page FM row.
  "fmtuner", nullptr,
  "FM tuner (live controls on lens; 'fmradioread' for CLI)",
  /*buildText=*/  g2BuildFmTunerInfo,
  /*showMenu=*/   nullptr,
  /*handleTap=*/  g2FmTunerHandleTap,
  G2_HIJACK_PAGE_FMRADIO,
  // 1 s tick: the fmRadio task polls the chip every 250 ms, so seek
  // progress and RDS updates render promptly; quiet ticks are free via
  // the byte-identical diff cache. Taps kick an immediate refresh.
  /*liveIntervalMs=*/ 1000,
  /*backLabel=*/      "<- Hardware",
  /*prefersTextWidget=*/ true,    // routes through g2StartLiveTextPage
  /*liveRender=*/     renderFmTunerLive,
};

bool g2ShowFmTunerPage() {
  if (!g2StartLiveTextPage(/*buildText=*/ nullptr,
                           kFmTunerPage.liveIntervalMs,
                           kFmTunerPage.liveRender)) {
    DEBUG_G2F("[G2] fmtuner: live-text spawn failed");
    return false;
  }
  g2SetHijackPage(G2_HIJACK_PAGE_FMRADIO);
  gFmLastReadout[0] = '\0';   // fresh readout on entry, don't suppress first tick
  BROADCAST_PRINTF("[G2] Hijack: FM tuner opened");
  return true;
}
#endif  // ENABLE_FM_RADIO

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
