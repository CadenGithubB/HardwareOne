// =============================================================================
// G2 glasses — "Camera Settings" sub-page implementation
// =============================================================================
// Tap-to-cycle settings list. Each setting in `kSettings[]` defines its
// value range and an apply hook that calls the existing cmd_camera*
// handler — that handler owns the RAM mutation, persistence, and live sensor
// update. The tap worker never pre-writes gSettings: doing so made the async
// handler see an equal value and skip setSetting's persistence path.
//
// The settings table also drives row rendering, so adding a new
// setting is a one-row table append.

#include "G2_Page_CameraSettings.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_CAMERA_SENSOR

#include "G2_Glasses.h"
#include "G2_Page_Common.h"
#include "G2_Page_Sensors.h"          // g2ShowSensorsMenu (back navigation)
#include "System_Camera_DVP.h"        // camera setting accessors for menu rows
#include "System_Settings.h"          // gSettings
#include "System_Debug.h"
#include "G2_HijackCmd.h"             // g2SubmitHijackCommand — Group A migration
#include <Arduino.h>
#include <new>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Settings table
// -----------------------------------------------------------------------------

// Cycle direction: tap advances by +1 with wrap. Opposite-direction
// cycling would need a second tap target — not worth it for the lens
// UX; if the user wants to step backward they tap (range-1) more
// times. Ranges below are intentionally small.

// Persistent setting IDs are stable; capabilities decide which rows exist.
// Keep the visible list in increasing pixel area across DVP and CSI backends.
static const int kFramesizeCycleOrder[] = {6, 7, 8, 9, 10, 0, 12, 1, 2, 3, 11, 4, 5};
static const size_t kFramesizeCycleCount =
    sizeof(kFramesizeCycleOrder) / sizeof(kFramesizeCycleOrder[0]);

static const char* framesizeLabel(int settingIdx) {
  const auto* info = cameraFrameSizeInfo(static_cast<CameraFrameSize>(settingIdx));
  return info ? info->name : "?";
}

// Return the setting ID for a supported, one-based picker row. Rendering and
// dispatch use the same mapping so a hidden resolution cannot shift a tap.
static int framesizeForRow(size_t displayRow, uint32_t resolutions) {
  if (!displayRow) return -1;
  for (int id : kFramesizeCycleOrder) {
    if (!(resolutions & cameraResolutionBit(static_cast<CameraFrameSize>(id)))) continue;
    if (--displayRow == 0) return id;
  }
  return -1;
}

// Page level — top-level category menu, one of three category sub-lists,
// or one of two picker sub-pages.
//
// Top is the entry point: user picks a category. Each category sub-list
// holds the related settings (Camera = exposure family; Transform = H/V
// flip; PostProc = quality + denoise — see CamCategory below for the
// full mapping). The two pickers (Resolution, Stream) are separate because
// they pick from a fixed list rather than cycling on tap.
enum CamSettingsLevel : uint8_t {
  CAM_LEVEL_TOP               = 0,
  CAM_LEVEL_SUB_CAMERA        = 1,  // Resolution, Brightness, Contrast, Exposure, Sharpness
  CAM_LEVEL_SUB_TRANSFORM     = 2,  // H Mirror, V Flip
  CAM_LEVEL_SUB_POSTPROC      = 3,  // Quality, Denoise, Tone Map
  CAM_LEVEL_RESOLUTION_PICKER = 4,  // entered from SUB_CAMERA
  CAM_LEVEL_STREAM_PICKER     = 5,  // entered from TOP (Stream size)
};
static CamSettingsLevel gLevel = CAM_LEVEL_TOP;

// Category tag for kSettings entries. Drives both the top-level menu and
// the per-category sub-list rendering: each sub-list iterates kSettings
// and includes only entries matching its category.
//
// Note on Denoise: technically applied by the OV3660's ISP during readout
// (so it's a sensor-side setting), but UX-wise users group it with quality
// knobs rather than exposure knobs, so it lives under PostProc with JPEG
// Quality.
enum CamCategory : uint8_t {
  CAM_CAT_CAMERA    = 0,
  CAM_CAT_TRANSFORM = 1,
  CAM_CAT_POSTPROC  = 2,
};

// Stream output sizes are independent of the camera's native aspect ratio.
// The shared image converter fits the source and adds bars when needed.
struct StreamSizePreset { int16_t w; int16_t h; const char* label; };
static const StreamSizePreset kStreamPresets[] = {
  // 4:3 output sizes
  {  -1,  -1, "-- 4:3 output --" },
  {  96,  72, "96x72 (Small)"        },
  { 128,  96, "128x96"               },
  { 192, 144, "192x144 (Full panel)" },

  // Square and wide output sizes
  {  -1,  -1, "-- Square / wide --" },
  {  96,  96, "96x96 (1:1)"   },
  { 128, 128, "128x128 (1:1)" },
  { 144, 144, "144x144 (1:1)" },
  { 160,  80, "160x80 (2:1)"  },
  { 192,  96, "192x96 (2:1)"  },
  { 240, 120, "240x120 (2:1)" },
  { 288, 144, "288x144 (Full 2:1)" },
};
static const size_t kStreamPresetCount =
    sizeof(kStreamPresets) / sizeof(kStreamPresets[0]);

static inline bool streamPresetIsHeader(const StreamSizePreset& p) {
  return p.w < 0 || p.h < 0;
}

// One row per exposed setting. cycle() takes the current value and
// returns the next; the dispatcher does the read-modify-write through
// the typed accessor so int and bool storage both work.
enum CamValueType : uint8_t {
  CV_INT  = 0,
  CV_BOOL = 1,
};

struct CamSetting {
  const char*  label;           // shown before ": <value>"
  CamValueType type;            // selects accessor (gSettings field is int vs bool)
  void*        valuePtr;        // typed by `type`
  int          (*cycle)(int curr);                 // returns next
  void         (*format)(char* out, size_t cap, int v); // render value
  bool         (*apply)(int v); // queue cmd_camera* with stringified value
  CamCategory  category;        // which sub-list this setting belongs to
  int          control = -1;    // CameraControl, or -1 for shared pipeline settings
};

// Typed read — bool storage stays bool in gSettings; we expose it as int
// (0/1) at the table layer so cycle/format/apply can be a single int shape.
static int readSetting(const CamSetting& s) {
  if (!s.valuePtr) return 0;
  if (s.type == CV_BOOL) return (*(bool*)s.valuePtr) ? 1 : 0;
  return *(int*)s.valuePtr;
}

static void redrawCurrentCameraSettings();

// Helpers ---------------------------------------------------------------------

// Group A: each camera-setting tap used to call cmd_camera*(arg) inline on
// tap_disp, which in turn does setSetting + writeSettingsJson + camera
// restart — the deepest stack chain in the hijack tap path. Submit through
// cmd_exec_task instead, then redraw from the committed gSettings value. This
// keeps flash I/O and mutation on the executor and makes queue failure a true
// no-op rather than leaving an unpersisted optimistic value in RAM.
static void onCameraSettingDone(bool ok, const char* result,
                                const G2CmdCookie& cookie, void* /*userData*/) {
  if (!ok || (result && strncmp(result, "Error", 5) == 0)) {
    DEBUG_G2F("[G2] Camera settings: command failed: %.80s",
              (result && result[0]) ? result : "no result");
  }

  RedrawSpec* spec = new (std::nothrow) RedrawSpec{};
  if (!spec) {
    DEBUG_G2F("[G2] Camera settings: redraw allocation failed");
    return;
  }
  spec->render = redrawCurrentCameraSettings;
  LensUiJob* job = new (std::nothrow) LensUiJob{};
  if (!job) {
    delete spec;
    DEBUG_G2F("[G2] Camera settings: redraw job allocation failed");
    return;
  }
  job->kind           = LensJobKind::Redraw;
  job->submitMenuGen  = cookie.menuGen;
  job->cmdSeq         = cookie.seq;
  job->targetPage     = cookie.targetPage;
  job->targetNetSub   = cookie.targetNetSub;
  job->payload.redraw = spec;
  if (!g2EnqueueLensJob(job)) {
    delete spec;
    delete job;
    DEBUG_G2F("[G2] Camera settings: redraw queue full");
  }
}

static bool applyByCmd(const char* cmdName, int v) {
  if (!cmdName || !*cmdName) return false;
  char line[40];
  snprintf(line, sizeof(line), "%s %d", cmdName, v);
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = (uint8_t)gLevel;
  if (!g2SubmitHijackCommand(line, cookie, onCameraSettingDone, nullptr)) {
    DEBUG_G2F("[G2] Camera settings: %s submit FAILED — no change made", cmdName);
    return false;
  }
  return true;
}

// Cycle helpers — each clamps an out-of-range incoming value into a
// canonical entry of its set, then advances by one with wrap.
static int cycleRangeM2P2(int v) {
  if (v < -2 || v > 2) v = 0;
  return (v == 2) ? -2 : (v + 1);
}

static int cycleDenoise(int v) {
  if (v < 0 || v > 8) v = 0;
  return (v == 8) ? 0 : (v + 1);
}

static int cycleBool(int v) {
  return (v != 0) ? 0 : 1;
}

static int cycleToneMap(int v) {
  // 0=Linear, 1=Balanced, 2=Shadows, 3=Legacy — lens stream 4-bpp algorithm.
  if (v < 0 || v > 3) v = 1;
  return (v == 3) ? 0 : (v + 1);
}

static int cycleFps(int v) {
  // Coarse stops only — lens taps don't need 1-fps granularity, and
  // getting to 15/20 should be a couple of taps. Same 1..20 clamp as
  // camerafps; off-grid values (e.g. set from web) advance to the next stop.
  static const int kStops[] = { 5, 10, 15, 20 };
  static const int kN = (int)(sizeof(kStops) / sizeof(kStops[0]));
  for (int i = 0; i < kN; i++) {
    if (kStops[i] == v) return kStops[(i + 1) % kN];
  }
  for (int i = 0; i < kN; i++) {
    if (kStops[i] > v) return kStops[i];
  }
  return kStops[0];
}

static void fmtToneMap(char* out, size_t cap, int v) {
  const char* name =
      (v == 0) ? "Linear" :
      (v == 2) ? "Shadows" :
      (v == 3) ? "Legacy" :
                 "Balanced";
  snprintf(out, cap, "%s", name);
}

static int cycleQuality(int v) {
  // 0..63, step 4 → 16 stops. Short enough to be usable on the lens
  // but spans the full range.
  if (v < 0 || v > 63) v = 12;
  v += 4;
  if (v > 60) v = 0;
  return v;
}

static int cycleFramesize(int v) {
  const auto caps = getCameraCapabilities();
  size_t start = kFramesizeCycleCount - 1;
  for (size_t i = 0; i < kFramesizeCycleCount; ++i) {
    if (kFramesizeCycleOrder[i] == v) { start = i; break; }
  }
  for (size_t offset = 1; offset <= kFramesizeCycleCount; ++offset) {
    int id = kFramesizeCycleOrder[(start + offset) % kFramesizeCycleCount];
    if (caps.resolutions & cameraResolutionBit(static_cast<CameraFrameSize>(id))) return id;
  }
  return v;
}

// Format helpers --------------------------------------------------------------

static void fmtSignedInt(char* out, size_t cap, int v) {
  // Show sign explicitly for ranges centred at 0 so the user can tell
  // -1 from 1 at a glance.
  snprintf(out, cap, "%+d", v);
}

static void fmtUnsignedInt(char* out, size_t cap, int v) {
  snprintf(out, cap, "%d", v);
}

static void fmtBool(char* out, size_t cap, int v) {
  snprintf(out, cap, "%s", v ? "ON" : "OFF");
}

static void fmtFramesize(char* out, size_t cap, int v) {
  snprintf(out, cap, "%s", framesizeLabel(v));
}

// Apply wrappers --------------------------------------------------------------
// Each forwards to the matching cmd_camera* handler so the persist +
// live-apply behaviour stays identical to CLI / web paths. Keeping a
// thin per-setting wrapper avoids leaking function pointers with
// String-arg signatures into the table.

static bool applyFramesize(int v)  { return applyByCmd("cameraframesize",  v); }
static bool applyBrightness(int v) { return applyByCmd("camerabrightness", v); }
static bool applyContrast(int v)   { return applyByCmd("cameracontrast",   v); }
static bool applyExposure(int v)   { return applyByCmd("cameraexposure",   v); }
static bool applySharpness(int v)  { return applyByCmd("camerasharpness",  v); }
static bool applyDenoise(int v)    { return applyByCmd("cameradenoise",    v); }
static bool applyHMirror(int v)    { return applyByCmd("camerahmirror",    v); }
static bool applyVFlip(int v)      { return applyByCmd("cameravflip",      v); }
static bool applyQuality(int v)    { return applyByCmd("cameraquality",    v); }
static bool applyToneMap(int v)    { return applyByCmd("g2streamtonemap",  v); }
static bool applyFps(int v)        { return applyByCmd("camerafps",        v); }

// Table -----------------------------------------------------------------------
// Order within each category = display order in that category's sub-list.
// Resolution leads the Camera category because it's the most-touched
// setting; Quality leads PostProc for the same reason.

static const CamSetting kSettings[] = {
  // Camera sub-list — sensor exposure / shaping knobs
  { "Resolution", CV_INT,  &gSettings.cameraFramesize,  cycleFramesize,  fmtFramesize,   applyFramesize,  CAM_CAT_CAMERA    },
  { "Brightness", CV_INT,  &gSettings.cameraBrightness, cycleRangeM2P2,  fmtSignedInt,   applyBrightness, CAM_CAT_CAMERA, static_cast<int>(CameraControl::Brightness) },
  { "Contrast",   CV_INT,  &gSettings.cameraContrast,   cycleRangeM2P2,  fmtSignedInt,   applyContrast,   CAM_CAT_CAMERA, static_cast<int>(CameraControl::Contrast) },
  { "Exposure",   CV_INT,  &gSettings.cameraAELevel,    cycleRangeM2P2,  fmtSignedInt,   applyExposure,   CAM_CAT_CAMERA, static_cast<int>(CameraControl::ExposureLevel) },
  { "Sharpness",  CV_INT,  &gSettings.cameraSharpness,  cycleRangeM2P2,  fmtSignedInt,   applySharpness,  CAM_CAT_CAMERA, static_cast<int>(CameraControl::Sharpness) },
  // Transform sub-list — geometric flips
  { "H Mirror",   CV_BOOL, &gSettings.cameraHMirror,    cycleBool,       fmtBool,        applyHMirror,    CAM_CAT_TRANSFORM, static_cast<int>(CameraControl::HMirror) },
  { "V Flip",     CV_BOOL, &gSettings.cameraVFlip,      cycleBool,       fmtBool,        applyVFlip,      CAM_CAT_TRANSFORM, static_cast<int>(CameraControl::VFlip) },
  // Post Processing sub-list — image-quality knobs (Denoise is sensor-side
  // technically, but UX-wise belongs with Quality not Brightness — see the
  // CamCategory enum doc above). Tone Map is the lens 4-bpp algorithm
  // (Linear / Balanced / Shadows / Legacy), not a sensor register.
  { "Quality",    CV_INT,  &gSettings.cameraQuality,    cycleQuality,    fmtUnsignedInt, applyQuality,    CAM_CAT_POSTPROC  },
  { "Denoise",    CV_INT,  &gSettings.cameraDenoise,    cycleDenoise,    fmtUnsignedInt, applyDenoise,    CAM_CAT_POSTPROC, static_cast<int>(CameraControl::Denoise) },
  { "Tone Map",   CV_INT,  &gSettings.g2StreamToneMap,  cycleToneMap,    fmtToneMap,     applyToneMap,    CAM_CAT_POSTPROC  },
};
static const size_t kSettingsCount = sizeof(kSettings) / sizeof(kSettings[0]);

static bool settingSupported(const CamSetting& setting, const CameraCapabilities& caps) {
  return setting.control < 0 ||
         (caps.controls & cameraControlBit(static_cast<CameraControl>(setting.control)));
}

// -----------------------------------------------------------------------------
// Row buffer
// -----------------------------------------------------------------------------

#define CAM_SETTINGS_ROW_LEN  32
// 1 back row + N settings rows. Also covers all 13 resolution IDs and the
// independent stream-size picker, with room for an unavailable-controls row.
static EXT_RAM_BSS_ATTR char gRows[1 + 16][CAM_SETTINGS_ROW_LEN];  // PSRAM: deep-copied by g2ShowListPage
static const char* gRowPtrs[1 + 16];
// Keep the rendered identity until the next redraw. Capabilities can change
// when the camera opens; a stale tap must never select a different setting.
static size_t gSettingRowMap[1 + 16];
static int gResolutionRowMap[1 + 16];
static size_t gSettingRowsCount = 0;
static size_t gResolutionRowsCount = 0;
static CamCategory gRenderedCategory = CAM_CAT_CAMERA;

// -----------------------------------------------------------------------------
// Forward decls — show/build helpers used across the level handlers
// -----------------------------------------------------------------------------

static size_t buildTopRows();
static size_t buildSubRows(CamCategory cat);
static size_t buildResolutionPickerRows();
static size_t buildStreamPickerRows();
static void   showSubMenu(CamCategory cat);
static void   showResolutionPicker();
static void   showStreamPicker();

// Recover the setting that was rendered in a one-based sub-list row.
// Current capabilities are checked again by the dispatcher before submission.
static size_t kSettingsIndexForSubRow(CamCategory cat, size_t displayRow) {
  if (cat != gRenderedCategory || !displayRow || displayRow >= gSettingRowsCount) return SIZE_MAX;
  return gSettingRowMap[displayRow];
}

// -----------------------------------------------------------------------------
// Render — top-level category menu
// -----------------------------------------------------------------------------

static size_t buildTopRows() {
  size_t row = 0;
  strncpy(gRows[row], "<- Camera", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  // Stream + FPS live at the top level (lens cadence knobs, not sensor
  // registers). Show current values inline; Stream opens a picker, FPS
  // cycles on tap (same camerafps setting as the web UI).
  snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "Stream: %dx%d >",
           gSettings.g2StreamWidth, gSettings.g2StreamHeight);
  gRowPtrs[row] = gRows[row]; row++;

  {
    int fps = gSettings.cameraStreamFps;
    if (fps < 1) fps = 1;
    if (fps > 20) fps = 20;
    snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "FPS: %d", fps);
    gRowPtrs[row] = gRows[row]; row++;
  }

  // Three category openers. Order is: most-touched first.
  strncpy(gRows[row], "Camera >", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row]; row++;

  strncpy(gRows[row], "Transform >", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row]; row++;

  strncpy(gRows[row], "Post Processing >", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row]; row++;

  return row;
}

// -----------------------------------------------------------------------------
// Render — category sub-list (Camera / Transform / PostProc)
// -----------------------------------------------------------------------------

static size_t buildSubRows(CamCategory cat) {
  const auto caps = getCameraCapabilities();
  gSettingRowsCount = 0;
  gRenderedCategory = cat;
  for (auto& index : gSettingRowMap) index = SIZE_MAX;
  size_t row = 0;
  strncpy(gRows[row], "<- Settings", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  for (size_t i = 0; i < kSettingsCount && row < (sizeof(gRows) / sizeof(gRows[0])); i++) {
    const CamSetting& s = kSettings[i];
    if (s.category != cat || !settingSupported(s, caps)) continue;
    char valueBuf[16];
    valueBuf[0] = '\0';
    if (s.format) s.format(valueBuf, sizeof(valueBuf), readSetting(s));
    if (strcmp(s.label, "Resolution") == 0 &&
        (readSetting(s) < 0 || readSetting(s) >= int(CameraFrameSize::Count) ||
         !(caps.resolutions & cameraResolutionBit(static_cast<CameraFrameSize>(readSetting(s)))))) {
      snprintf(valueBuf, sizeof(valueBuf), "%s [N/A]", framesizeLabel(readSetting(s)));
    }
    // Resolution is the one row that opens a picker instead of cycling —
    // keep the ">" affordance so the user knows tap behaviour differs.
    if (strcmp(s.label, "Resolution") == 0) {
      snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "Resolution: %s >", valueBuf);
    } else {
      snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "%s: %s", s.label, valueBuf);
    }
    gRowPtrs[row] = gRows[row];
    gSettingRowMap[row] = i;
    row++;
  }
  if (row == 1) {
    snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "No supported controls");
    gRowPtrs[row] = gRows[row];
    row++;
  }
  gSettingRowsCount = row;
  return row;
}

// -----------------------------------------------------------------------------
// Render — resolution picker sub-page (entered from Camera sub-list)
// -----------------------------------------------------------------------------

static size_t buildResolutionPickerRows() {
  const auto caps = getCameraCapabilities();
  gResolutionRowsCount = 0;
  for (auto& id : gResolutionRowMap) id = -1;
  const int current = (int)gSettings.cameraFramesize;

  size_t row = 0;
  strncpy(gRows[row], "<- Camera", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  for (size_t displayRow = 1; row < (sizeof(gRows) / sizeof(gRows[0])); ++displayRow) {
    const int id = framesizeForRow(displayRow, caps.resolutions);
    if (id < 0) break;
    const auto* info = cameraFrameSizeInfo(static_cast<CameraFrameSize>(id));
    if (!info) break;
    snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "%s%s %ux%u",
             id == current ? "[X] " : "    ", info->name,
             (unsigned)info->width, (unsigned)info->height);
    gRowPtrs[row] = gRows[row];
    gResolutionRowMap[row] = id;
    row++;
  }
  gResolutionRowsCount = row;
  return row;
}

static void showResolutionPicker() {
  gLevel = CAM_LEVEL_RESOLUTION_PICKER;
  size_t n = buildResolutionPickerRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    DEBUG_G2F("[G2] Camera settings: resolution picker shown (%u rows)",
              (unsigned)n);
  }
}

// -----------------------------------------------------------------------------
// Render — stream-size picker sub-page (entered from top-level)
// -----------------------------------------------------------------------------

static size_t buildStreamPickerRows() {
  const int curW = gSettings.g2StreamWidth;
  const int curH = gSettings.g2StreamHeight;

  size_t row = 0;
  strncpy(gRows[row], "<- Settings", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  for (size_t i = 0; i < kStreamPresetCount && row < (sizeof(gRows) / sizeof(gRows[0])); i++) {
    const StreamSizePreset& p = kStreamPresets[i];
    if (streamPresetIsHeader(p)) {
      // Section header — render the label only, no [X] gutter, no
      // selection state. Tap handler ignores rows that hit a header.
      snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "%s", p.label);
      gRowPtrs[row] = gRows[row];
      row++;
      continue;
    }
    const bool selected = (p.w == curW && p.h == curH);
    snprintf(gRows[row], CAM_SETTINGS_ROW_LEN,
             "%s%s",
             selected ? "[X] " : "    ",
             p.label);
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

static void showStreamPicker() {
  gLevel = CAM_LEVEL_STREAM_PICKER;
  size_t n = buildStreamPickerRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    DEBUG_G2F("[G2] Camera settings: stream picker shown (%u rows)",
              (unsigned)n);
  }
}

// -----------------------------------------------------------------------------
// Sub-menu show helpers — set level + render
// -----------------------------------------------------------------------------

static void showSubMenu(CamCategory cat) {
  switch (cat) {
    case CAM_CAT_CAMERA:    gLevel = CAM_LEVEL_SUB_CAMERA;    break;
    case CAM_CAT_TRANSFORM: gLevel = CAM_LEVEL_SUB_TRANSFORM; break;
    case CAM_CAT_POSTPROC:  gLevel = CAM_LEVEL_SUB_POSTPROC;  break;
  }
  size_t n = buildSubRows(cat);
  if (g2ShowListPage(gRowPtrs, n)) {
    DEBUG_G2F("[G2] Camera settings: sub-menu cat=%u shown (%u rows)",
              (unsigned)cat, (unsigned)n);
  }
}

static void showTopMenu() {
  gLevel = CAM_LEVEL_TOP;
  size_t n = buildTopRows();
  g2ShowListPage(gRowPtrs, n);
}

// Runs only on the lens-applier worker. If the user moved within Camera
// Settings while cmd_exec was working, refresh the level they are on now;
// if they left the page, do nothing. A full page transition also bumps the
// shared menu generation, so the Redraw job is normally dropped even earlier.
static void redrawCurrentCameraSettings() {
  if (g2GetHijackPage() != G2_HIJACK_PAGE_CAMERA_SETTINGS) return;
  switch (gLevel) {
    case CAM_LEVEL_TOP:               showTopMenu();                         return;
    case CAM_LEVEL_SUB_CAMERA:        showSubMenu(CAM_CAT_CAMERA);           return;
    case CAM_LEVEL_SUB_TRANSFORM:     showSubMenu(CAM_CAT_TRANSFORM);        return;
    case CAM_LEVEL_SUB_POSTPROC:      showSubMenu(CAM_CAT_POSTPROC);         return;
    case CAM_LEVEL_RESOLUTION_PICKER: showResolutionPicker();                return;
    case CAM_LEVEL_STREAM_PICKER:     showStreamPicker();                    return;
  }
}

// -----------------------------------------------------------------------------
// CLI text — flat dump of every setting plus Stream
// -----------------------------------------------------------------------------

void g2BuildCameraSettingsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  const auto caps = getCameraCapabilities();
  size_t pos = 0;

  auto append = [&](const char* line) {
    if (pos >= cap - 1) return;
    int w = snprintf(out + pos, cap - pos, "%s\n", line);
    if (w > 0) pos += ((size_t)w < cap - pos) ? (size_t)w : cap - pos - 1;
  };

  // Stream + FPS come first — both live at the top level on the lens.
  char line[64];
  snprintf(line, sizeof(line), "Stream: %dx%d",
           gSettings.g2StreamWidth, gSettings.g2StreamHeight);
  append(line);
  {
    int fps = gSettings.cameraStreamFps;
    if (fps < 1) fps = 1;
    if (fps > 20) fps = 20;
    snprintf(line, sizeof(line), "FPS: %d", fps);
    append(line);
  }

  // Then every camera setting, grouped by category. Iterate the table
  // three times (once per category) so the dump preserves the lens UI's
  // grouping rather than table declaration order.
  static const struct { CamCategory cat; const char* header; } kGroups[] = {
    { CAM_CAT_CAMERA,    "[Camera]"          },
    { CAM_CAT_TRANSFORM, "[Transform]"       },
    { CAM_CAT_POSTPROC,  "[Post Processing]" },
  };
  for (size_t g = 0; g < sizeof(kGroups) / sizeof(kGroups[0]); g++) {
    append(kGroups[g].header);
    for (size_t i = 0; i < kSettingsCount; i++) {
      const CamSetting& s = kSettings[i];
      if (s.category != kGroups[g].cat || !settingSupported(s, caps)) continue;
      char valueBuf[16];
      valueBuf[0] = '\0';
      if (s.format) s.format(valueBuf, sizeof(valueBuf), readSetting(s));
      if (strcmp(s.label, "Resolution") == 0 &&
          (readSetting(s) < 0 || readSetting(s) >= int(CameraFrameSize::Count) ||
           !(caps.resolutions & cameraResolutionBit(static_cast<CameraFrameSize>(readSetting(s)))))) {
        snprintf(valueBuf, sizeof(valueBuf), "%s [N/A]", framesizeLabel(readSetting(s)));
      }
      snprintf(line, sizeof(line), "  %s: %s", s.label, valueBuf);
      append(line);
    }
  }

  if (pos < cap) out[pos] = '\0';
}

// -----------------------------------------------------------------------------
// Public — show / handle taps
// -----------------------------------------------------------------------------

void g2ShowCameraSettingsMenu() {
  // Always enter at the TOP level — never resume on a sub-list. Entering
  // the page means "open camera settings", not "go to wherever I was".
  gLevel = CAM_LEVEL_TOP;
  size_t n = buildTopRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_CAMERA_SETTINGS);
    DEBUG_G2F("[G2] Camera settings shown (%u rows)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Camera settings show FAILED");
  }
}

// -----------------------------------------------------------------------------
// Tap handlers — one per level
// -----------------------------------------------------------------------------

static void handleTopTap(uint32_t idx) {
  if (idx == 0) {
    // Stream-relaunch hook. When the camera-stream worker chained into
    // this page via its "Settings >>" row, it set
    // g2CamStreamSettingsExitRelaunch = true so back returns to the
    // live stream rather than the CAM detail list — the user came
    // here from the stream and presumably wants to see their settings
    // apply on it. Clear the flag first so a future direct entry to
    // this page (CAM detail → CAM Settings) doesn't inherit it.
    if (g2CamStreamSettingsExitRelaunch) {
      g2CamStreamSettingsExitRelaunch = false;
      DEBUG_G2F("[G2] Camera settings: back → relaunch stream");
      // onDone returns to CAM detail just like a fresh stream entry
      // would — same helper Sensors-detail uses to wire CAM Stream.
      g2ShowCameraStream([]() { g2ReshowSensorsDetail(); });
      return;
    }
    // Back to the CAM detail page (NOT the sensors landing list). Same
    // helper Camera Settings already used pre-reorg.
    g2ReshowSensorsDetail();
    return;
  }
  // Layout: 0 back / 1 Stream / 2 FPS / 3 Camera / 4 Transform / 5 PostProc
  switch (idx) {
    case 1: showStreamPicker();          return;
    case 2: {
      const int prev = gSettings.cameraStreamFps;
      const int next = cycleFps(prev);
      if (applyFps(next)) {
        DEBUG_G2F("[G2] Camera settings: FPS %d → %d queued", prev, next);
      }
      showTopMenu();
      return;
    }
    case 3: showSubMenu(CAM_CAT_CAMERA); return;
    case 4: showSubMenu(CAM_CAT_TRANSFORM); return;
    case 5: showSubMenu(CAM_CAT_POSTPROC);  return;
    default:
      DEBUG_G2F("[G2] Camera settings: top-level tap idx=%u out of range",
                (unsigned)idx);
      return;
  }
}

static void handleSubTap(CamCategory cat, uint32_t idx) {
  if (idx == 0) {
    // Back to the top-level menu without changing anything.
    showTopMenu();
    return;
  }
  size_t k = kSettingsIndexForSubRow(cat, idx);
  if (k == SIZE_MAX) {
    DEBUG_G2F("[G2] Camera settings: sub tap idx=%u out of range (cat=%u)",
              (unsigned)idx, (unsigned)cat);
    return;
  }
  const CamSetting& s = kSettings[k];
  if (!settingSupported(s, getCameraCapabilities()) || !s.valuePtr || !s.cycle || !s.apply) return;

  // Resolution opens the framesize picker rather than cycling — same
  // affordance the row's ">" indicator hints at.
  if (strcmp(s.label, "Resolution") == 0) {
    showResolutionPicker();
    return;
  }

  const int prev = readSetting(s);
  const int next = s.cycle(prev);
  const bool queued = s.apply(next);  // executor owns mutate + persist + live apply

  char dispBuf[16] = {0};
  if (s.format) s.format(dispBuf, sizeof(dispBuf), next);
  if (queued) {
    BROADCAST_PRINTF("[G2] Camera settings: %s %d -> %s queued",
                     s.label ? s.label : "?", prev, dispBuf);
  }

  // Re-render the same sub-list so the row reflects the new value.
  size_t n = buildSubRows(cat);
  g2ShowListPage(gRowPtrs, n);
}

static void handleResolutionPickerTap(uint32_t idx) {
  if (idx == 0) {
    // Back to the Camera sub-list (not all the way to top — that would
    // bury the Resolution row the user just navigated TO).
    showSubMenu(CAM_CAT_CAMERA);
    return;
  }
  const int newSetting = idx < gResolutionRowsCount ? gResolutionRowMap[idx] : -1;
  if (newSetting < 0 || !cameraSupportsResolution(static_cast<CameraFrameSize>(newSetting))) {
    DEBUG_G2F("[G2] Camera settings: unsupported resolution picker row %u", (unsigned)idx);
    return;
  }
  const int prevSetting = (int)gSettings.cameraFramesize;
  if (newSetting == prevSetting) {
    BROADCAST_PRINTF("[G2] Camera settings: resolution unchanged (%s)",
                     framesizeLabel(newSetting));
  } else {
    if (applyFramesize(newSetting)) {
      BROADCAST_PRINTF("[G2] Camera settings: resolution %s -> %s queued",
                       framesizeLabel(prevSetting),
                       framesizeLabel(newSetting));
    }
  }
  // Return to the Camera sub-list with the new value reflected.
  showSubMenu(CAM_CAT_CAMERA);
}

static void handleStreamPickerTap(uint32_t idx) {
  if (idx == 0) {
    // Back to the top-level menu (Stream lives at top, not in a sub-list).
    showTopMenu();
    return;
  }
  const size_t i = (size_t)idx - 1;
  if (i >= kStreamPresetCount) {
    DEBUG_G2F("[G2] Camera settings: stream picker tap idx=%u out of range (count=%u)",
              (unsigned)idx, (unsigned)kStreamPresetCount);
    return;
  }
  const StreamSizePreset& p = kStreamPresets[i];
  if (streamPresetIsHeader(p)) {
    // Tap on a section header — no-op; just re-render so the user sees
    // the picker is still active.
    DEBUG_G2F("[G2] Camera settings: stream picker tap on header row idx=%u — ignored",
              (unsigned)idx);
    showStreamPicker();
    return;
  }
  const int prevW = gSettings.g2StreamWidth;
  const int prevH = gSettings.g2StreamHeight;
  if (p.w == prevW && p.h == prevH) {
    BROADCAST_PRINTF("[G2] Camera settings: stream size unchanged (%dx%d)",
                     prevW, prevH);
  } else {
    char line[40];
    snprintf(line, sizeof(line), "g2streamres %dx%d", (int)p.w, (int)p.h);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = (uint8_t)gLevel;
    if (!g2SubmitHijackCommand(line, cookie, onCameraSettingDone, nullptr)) {
      DEBUG_G2F("[G2] Camera settings: g2streamres submit FAILED — "
                "no change made");
    } else {
      BROADCAST_PRINTF("[G2] Camera settings: stream size %dx%d -> %dx%d",
                       prevW, prevH, (int)p.w, (int)p.h);
    }
  }
  // Return to the top-level menu with the new value reflected.
  showTopMenu();
}

void g2CameraSettingsHandleTap(uint32_t idx) {
  switch (gLevel) {
    case CAM_LEVEL_TOP:               handleTopTap(idx);                 return;
    case CAM_LEVEL_SUB_CAMERA:        handleSubTap(CAM_CAT_CAMERA, idx); return;
    case CAM_LEVEL_SUB_TRANSFORM:     handleSubTap(CAM_CAT_TRANSFORM, idx); return;
    case CAM_LEVEL_SUB_POSTPROC:      handleSubTap(CAM_CAT_POSTPROC, idx);  return;
    case CAM_LEVEL_RESOLUTION_PICKER: handleResolutionPickerTap(idx);    return;
    case CAM_LEVEL_STREAM_PICKER:     handleStreamPickerTap(idx);        return;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_CAMERA_SENSOR
