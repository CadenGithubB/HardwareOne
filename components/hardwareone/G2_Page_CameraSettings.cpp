// =============================================================================
// G2 glasses — "Camera Settings" sub-page implementation
// =============================================================================
// Tap-to-cycle settings list. Each setting in `kSettings[]` defines its
// value range and an apply hook that calls the existing cmd_camera*
// handler — that handler does the persist (setSetting → JSON write)
// and the live sensor update. We just snprintf the new int into a
// String and pass it through. Result strings from the cmd_* handlers
// are ignored; their broadcast/log side effects are enough.
//
// The settings table also drives row rendering, so adding a new
// setting is a one-row table append.

#include "G2_Page_CameraSettings.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_CAMERA_SENSOR

#include "G2_Glasses.h"
#include "G2_Page_Common.h"
#include "G2_Page_Sensors.h"          // g2ShowSensorsMenu (back navigation)
#include "System_Camera_DVP.h"        // cmd_camera* handlers (inline fallback only)
#include "System_Settings.h"          // gSettings
#include "System_Debug.h"
#include "G2_HijackCmd.h"             // g2SubmitHijackCommand — Group A migration
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Settings table
// -----------------------------------------------------------------------------

// Cycle direction: tap advances by +1 with wrap. Opposite-direction
// cycling would need a second tap target — not worth it for the lens
// UX; if the user wants to step backward they tap (range-1) more
// times. Ranges below are intentionally small.

// Framesize is non-monotonic in the setting-index space (0..5 are
// QVGA..UXGA; 6..10 are 96x96..240x240). For the cycle order we want
// small→large visual ordering, so we map through this canonical list:
//
// Setting index 6 (96x96) is intentionally OMITTED from the picker.
// Field testing 2026-05-01 confirmed it produces JPEG frames that
// blow the OV3660's auto-sized frame buffer, locking the camera in
// FB-OVF and bricking the device at next boot (boot guard reverts
// it to QVGA — see initCamera()). Re-add only after the buffer-size
// problem is solved upstream.
static const int kFramesizeCycleOrder[] = {
  // 6,  // 96x96 — disabled (FB-OVF on OV3660, see comment above)
  7,  // 160x120 (QQVGA)
  8,  // 176x144 (QCIF)
  9,  // 240x176 (HQVGA)
  10, // 240x240
  0,  // 320x240 (QVGA)
  1,  // 640x480 (VGA)
  2,  // 800x600 (SVGA)
  3,  // 1024x768 (XGA)
  4,  // 1280x1024 (SXGA)
  5,  // 1600x1200 (UXGA)
};
static const size_t kFramesizeCycleCount =
    sizeof(kFramesizeCycleOrder) / sizeof(kFramesizeCycleOrder[0]);

static const char* framesizeLabel(int settingIdx) {
  switch (settingIdx) {
    case 0:  return "QVGA";
    case 1:  return "VGA";
    case 2:  return "SVGA";
    case 3:  return "XGA";
    case 4:  return "SXGA";
    case 5:  return "UXGA";
    case 6:  return "96x96";
    case 7:  return "QQVGA";
    case 8:  return "QCIF";
    case 9:  return "HQVGA";
    case 10: return "240x240";
    default: return "?";
  }
}

// Picker rows show "<LABEL> <WxH>" so the user sees both the standard
// name and the actual dimensions on one line. For sizes whose label is
// already the dimensions (96x96, 240x240) the helper returns just the
// label to avoid silly duplication ("96x96 96x96").
static const char* framesizeFullName(int settingIdx) {
  switch (settingIdx) {
    case 0:  return "QVGA 320x240";
    case 1:  return "VGA 640x480";
    case 2:  return "SVGA 800x600";
    case 3:  return "XGA 1024x768";
    case 4:  return "SXGA 1280x1024";
    case 5:  return "UXGA 1600x1200";
    case 6:  return "96x96";
    case 7:  return "QQVGA 160x120";
    case 8:  return "QCIF 176x144";
    case 9:  return "HQVGA 240x176";
    case 10: return "240x240";
    default: return "?";
  }
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
  CAM_LEVEL_SUB_POSTPROC      = 3,  // Quality, Denoise
  CAM_LEVEL_RESOLUTION_PICKER = 4,  // entered from SUB_CAMERA
  CAM_LEVEL_STREAM_PICKER     = 5,  // entered from TOP
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

// Stream size presets — each is W,H plus a label. The list is split into
// two visual sections by header rows (w/h = -1 sentinels): the camera is
// 4:3 today, so 4:3-aspect dst sizes fill the panel slot with no black
// bars (and with smaller wire payloads than 1:1 / 2:1 alternatives at
// the same panel area). The "with bars" section keeps the historical
// 1:1 and 2:1 sizes for users who want square/wide framing and accept
// the bandwidth cost of the bars (~25% of payload at 1:1, ~33% at 2:1
// against a 4:3 camera). Reference cadence comments are in
// G2_Glasses.cpp's camera-stream worker.
struct StreamSizePreset { int16_t w; int16_t h; const char* label; };
static const StreamSizePreset kStreamPresets[] = {
  // ── Fill (4:3, no bars with current camera) ──
  {  -1,  -1, "-- Fill (no bars) --" },
  {  96,  72, "96x72 (Small)"        },
  { 128,  96, "128x96"               },
  { 192, 144, "192x144 (Full panel)" },

  // ── With bars/pillars (against 4:3 camera) ──
  {  -1,  -1, "-- With bars/pillars --" },
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
  void         (*apply)(int v); // call cmd_camera* with stringified value
  CamCategory  category;        // which sub-list this setting belongs to
};

// Typed read/write — bool storage stays bool in gSettings; we expose
// it as int (0/1) at the table layer so cycle/format/apply can be a
// single int-based shape.
static int readSetting(const CamSetting& s) {
  if (!s.valuePtr) return 0;
  if (s.type == CV_BOOL) return (*(bool*)s.valuePtr) ? 1 : 0;
  return *(int*)s.valuePtr;
}
static void writeSetting(const CamSetting& s, int v) {
  if (!s.valuePtr) return;
  if (s.type == CV_BOOL) *(bool*)s.valuePtr = (v != 0);
  else                   *(int*)s.valuePtr  = v;
}

// Helpers ---------------------------------------------------------------------

// Group A: each camera-setting tap used to call cmd_camera*(arg) inline on
// tap_disp, which in turn does setSetting + writeSettingsJson + camera
// restart — the deepest stack chain in the hijack tap path. Now we submit
// the command line ("camera<thing> N") through cmd_exec_task, which has its
// own 24 KB stack and handles the persist + apply off the tap dispatcher.
// No callback: writeSetting() above already updated RAM and the inline
// g2ShowListPage re-render reads the new value, so the lens shows the new
// setting immediately. The persist/apply happens shortly after on cmd_exec.
static void applyByCmd(const char* cmdName, int v) {
  if (!cmdName || !*cmdName) return;
  char line[40];
  snprintf(line, sizeof(line), "%s %d", cmdName, v);
  G2CmdCookie cookie{};
  cookie.targetPage   = g2GetHijackPage();
  cookie.targetNetSub = 0;
  if (!g2SubmitHijackCommand(line, cookie, nullptr, nullptr)) {
    DEBUG_G2F("[G2] Camera settings: %s submit FAILED — feature won't apply", cmdName);
  }
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

static int cycleQuality(int v) {
  // 0..63, step 4 → 16 stops. Short enough to be usable on the lens
  // but spans the full range.
  if (v < 0 || v > 63) v = 12;
  v += 4;
  if (v > 60) v = 0;
  return v;
}

static int cycleFramesize(int v) {
  // Find current value in the canonical small→large order, advance by
  // one, wrap. If the stored value isn't in the table (defensive),
  // start at the first entry.
  int idx = 0;
  for (size_t i = 0; i < kFramesizeCycleCount; i++) {
    if (kFramesizeCycleOrder[i] == v) { idx = (int)i; break; }
  }
  idx = (idx + 1) % (int)kFramesizeCycleCount;
  return kFramesizeCycleOrder[idx];
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

static void applyFramesize(int v)  { applyByCmd("cameraframesize",  v); }
static void applyBrightness(int v) { applyByCmd("camerabrightness", v); }
static void applyContrast(int v)   { applyByCmd("cameracontrast",   v); }
static void applyExposure(int v)   { applyByCmd("cameraexposure",   v); }
static void applySharpness(int v)  { applyByCmd("camerasharpness",  v); }
static void applyDenoise(int v)    { applyByCmd("cameradenoise",    v); }
static void applyHMirror(int v)    { applyByCmd("camerahmirror",    v); }
static void applyVFlip(int v)      { applyByCmd("cameravflip",      v); }
static void applyQuality(int v)    { applyByCmd("cameraquality",    v); }

// Table -----------------------------------------------------------------------
// Order within each category = display order in that category's sub-list.
// Resolution leads the Camera category because it's the most-touched
// setting; Quality leads PostProc for the same reason.

static const CamSetting kSettings[] = {
  // Camera sub-list — sensor exposure / shaping knobs
  { "Resolution", CV_INT,  &gSettings.cameraFramesize,  cycleFramesize,  fmtFramesize,   applyFramesize,  CAM_CAT_CAMERA    },
  { "Brightness", CV_INT,  &gSettings.cameraBrightness, cycleRangeM2P2,  fmtSignedInt,   applyBrightness, CAM_CAT_CAMERA    },
  { "Contrast",   CV_INT,  &gSettings.cameraContrast,   cycleRangeM2P2,  fmtSignedInt,   applyContrast,   CAM_CAT_CAMERA    },
  { "Exposure",   CV_INT,  &gSettings.cameraAELevel,    cycleRangeM2P2,  fmtSignedInt,   applyExposure,   CAM_CAT_CAMERA    },
  { "Sharpness",  CV_INT,  &gSettings.cameraSharpness,  cycleRangeM2P2,  fmtSignedInt,   applySharpness,  CAM_CAT_CAMERA    },
  // Transform sub-list — geometric flips
  { "H Mirror",   CV_BOOL, &gSettings.cameraHMirror,    cycleBool,       fmtBool,        applyHMirror,    CAM_CAT_TRANSFORM },
  { "V Flip",     CV_BOOL, &gSettings.cameraVFlip,      cycleBool,       fmtBool,        applyVFlip,      CAM_CAT_TRANSFORM },
  // Post Processing sub-list — image-quality knobs (Denoise is sensor-side
  // technically, but UX-wise belongs with Quality not Brightness — see the
  // CamCategory enum doc above).
  { "Quality",    CV_INT,  &gSettings.cameraQuality,    cycleQuality,    fmtUnsignedInt, applyQuality,    CAM_CAT_POSTPROC  },
  { "Denoise",    CV_INT,  &gSettings.cameraDenoise,    cycleDenoise,    fmtUnsignedInt, applyDenoise,    CAM_CAT_POSTPROC  },
};
static const size_t kSettingsCount = sizeof(kSettings) / sizeof(kSettings[0]);

// -----------------------------------------------------------------------------
// Row buffer
// -----------------------------------------------------------------------------

#define CAM_SETTINGS_ROW_LEN  32
// 1 back row + N settings rows. The picker page needs 1 back row + 11
// resolution rows = 12, so size the shared buffer for that.
static EXT_RAM_BSS_ATTR char gRows[1 + 16][CAM_SETTINGS_ROW_LEN];  // PSRAM: deep-copied by g2ShowListPage
static const char* gRowPtrs[1 + 16];

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

// Map a sub-list display row (1-based, after the back row) to a kSettings
// index. Walks kSettings filtering on category until the Nth match. Returns
// SIZE_MAX if the row is out of range. Used by both buildSubRows (for
// rendering) and handleSubTap (for dispatching).
static size_t kSettingsIndexForSubRow(CamCategory cat, size_t displayRow) {
  size_t matched = 0;
  for (size_t i = 0; i < kSettingsCount; i++) {
    if (kSettings[i].category != cat) continue;
    matched++;
    if (matched == displayRow) return i;
  }
  return SIZE_MAX;
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

  // Stream lives at the top level (lens display size, not a camera setting).
  // Show current value inline so the user sees what's set without opening.
  snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "Stream: %dx%d >",
           gSettings.g2StreamWidth, gSettings.g2StreamHeight);
  gRowPtrs[row] = gRows[row]; row++;

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
  size_t row = 0;
  strncpy(gRows[row], "<- Settings", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  for (size_t i = 0; i < kSettingsCount && row < (sizeof(gRows) / sizeof(gRows[0])); i++) {
    const CamSetting& s = kSettings[i];
    if (s.category != cat) continue;
    char valueBuf[16];
    valueBuf[0] = '\0';
    if (s.format) s.format(valueBuf, sizeof(valueBuf), readSetting(s));
    // Resolution is the one row that opens a picker instead of cycling —
    // keep the ">" affordance so the user knows tap behaviour differs.
    if (strcmp(s.label, "Resolution") == 0) {
      snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "Resolution: %s >", valueBuf);
    } else {
      snprintf(gRows[row], CAM_SETTINGS_ROW_LEN, "%s: %s", s.label, valueBuf);
    }
    gRowPtrs[row] = gRows[row];
    row++;
  }
  return row;
}

// -----------------------------------------------------------------------------
// Render — resolution picker sub-page (entered from Camera sub-list)
// -----------------------------------------------------------------------------

static size_t buildResolutionPickerRows() {
  const int current = (int)gSettings.cameraFramesize;

  size_t row = 0;
  strncpy(gRows[row], "<- Camera", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  for (size_t i = 0; i < kFramesizeCycleCount && row < (sizeof(gRows) / sizeof(gRows[0])); i++) {
    const int idx = kFramesizeCycleOrder[i];
    const bool selected = (idx == current);
    snprintf(gRows[row], CAM_SETTINGS_ROW_LEN,
             "%s%s",
             selected ? "[X] " : "    ",
             framesizeFullName(idx));
    gRowPtrs[row] = gRows[row];
    row++;
  }
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

// -----------------------------------------------------------------------------
// CLI text — flat dump of every setting plus Stream
// -----------------------------------------------------------------------------

void g2BuildCameraSettingsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  size_t pos = 0;

  auto append = [&](const char* line) {
    int w = snprintf(out + pos, cap - pos, "%s\n", line);
    if (w > 0) pos += (size_t)w;
  };

  // Stream comes first — it's at the top level on the lens too.
  char line[64];
  snprintf(line, sizeof(line), "Stream: %dx%d",
           gSettings.g2StreamWidth, gSettings.g2StreamHeight);
  append(line);

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
      if (s.category != kGroups[g].cat) continue;
      char valueBuf[16];
      valueBuf[0] = '\0';
      if (s.format) s.format(valueBuf, sizeof(valueBuf), readSetting(s));
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
  // Layout: 0 back / 1 Stream / 2 Camera / 3 Transform / 4 PostProc
  switch (idx) {
    case 1: showStreamPicker();          return;
    case 2: showSubMenu(CAM_CAT_CAMERA); return;
    case 3: showSubMenu(CAM_CAT_TRANSFORM); return;
    case 4: showSubMenu(CAM_CAT_POSTPROC);  return;
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
  if (!s.valuePtr || !s.cycle || !s.apply) return;

  // Resolution opens the framesize picker rather than cycling — same
  // affordance the row's ">" indicator hints at.
  if (strcmp(s.label, "Resolution") == 0) {
    showResolutionPicker();
    return;
  }

  const int prev = readSetting(s);
  const int next = s.cycle(prev);
  writeSetting(s, next);   // RAM first so re-render shows the new value
  s.apply(next);           // persist + live-apply via cmd_camera*

  char dispBuf[16] = {0};
  if (s.format) s.format(dispBuf, sizeof(dispBuf), next);
  BROADCAST_PRINTF("[G2] Camera settings: %s %d -> %s",
                   s.label ? s.label : "?", prev, dispBuf);

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
  const size_t i = (size_t)idx - 1;
  if (i >= kFramesizeCycleCount) {
    DEBUG_G2F("[G2] Camera settings: resolution picker tap idx=%u out of range (count=%u)",
              (unsigned)idx, (unsigned)kFramesizeCycleCount);
    return;
  }
  const int newSetting = kFramesizeCycleOrder[i];
  const int prevSetting = (int)gSettings.cameraFramesize;
  if (newSetting == prevSetting) {
    BROADCAST_PRINTF("[G2] Camera settings: resolution unchanged (%s)",
                     framesizeFullName(newSetting));
  } else {
    BROADCAST_PRINTF("[G2] Camera settings: resolution %s -> %s",
                     framesizeFullName(prevSetting),
                     framesizeFullName(newSetting));
    applyFramesize(newSetting);
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
    // RAM update first so the re-render below sees the new value;
    // cmd_g2streamres persists asynchronously via setSetting.
    gSettings.g2StreamWidth  = p.w;
    gSettings.g2StreamHeight = p.h;

    char line[40];
    snprintf(line, sizeof(line), "g2streamres %dx%d", (int)p.w, (int)p.h);
    G2CmdCookie cookie{};
    cookie.targetPage   = g2GetHijackPage();
    cookie.targetNetSub = 0;
    if (!g2SubmitHijackCommand(line, cookie, nullptr, nullptr)) {
      DEBUG_G2F("[G2] Camera settings: g2streamres submit FAILED — "
                "RAM updated but persist won't happen");
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
