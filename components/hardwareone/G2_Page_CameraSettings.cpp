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
#include "System_Camera_DVP.h"        // cmd_camera* handlers
#include "System_Settings.h"          // gSettings
#include "System_Debug.h"
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

// Page level — main settings list, or the resolution-picker sub-list
// reached by tapping the Resolution row.
enum CamSettingsLevel : uint8_t {
  CAM_LEVEL_LIST              = 0,
  CAM_LEVEL_RESOLUTION_PICKER = 1,
};
static CamSettingsLevel gLevel = CAM_LEVEL_LIST;

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

static void applyByCmd(const char* (*cmd)(const String&), int v) {
  if (!cmd) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", v);
  String arg(buf);
  (void)cmd(arg);
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

static void applyFramesize(int v)  { applyByCmd(cmd_cameraframesize,  v); }
static void applyBrightness(int v) { applyByCmd(cmd_camerabrightness, v); }
static void applyContrast(int v)   { applyByCmd(cmd_cameracontrast,   v); }
static void applyExposure(int v)   { applyByCmd(cmd_cameraexposure,   v); }
static void applySharpness(int v)  { applyByCmd(cmd_camerasharpness,  v); }
static void applyDenoise(int v)    { applyByCmd(cmd_cameradenoise,    v); }
static void applyHMirror(int v)    { applyByCmd(cmd_camerahmirror,    v); }
static void applyVFlip(int v)      { applyByCmd(cmd_cameravflip,      v); }
static void applyQuality(int v)    { applyByCmd(cmd_cameraquality,    v); }

// Table -----------------------------------------------------------------------
// Order = display order on the page.

static const CamSetting kSettings[] = {
  { "Resolution", CV_INT,  &gSettings.cameraFramesize,  cycleFramesize,  fmtFramesize,   applyFramesize  },
  { "Brightness", CV_INT,  &gSettings.cameraBrightness, cycleRangeM2P2,  fmtSignedInt,   applyBrightness },
  { "Contrast",   CV_INT,  &gSettings.cameraContrast,   cycleRangeM2P2,  fmtSignedInt,   applyContrast   },
  { "Exposure",   CV_INT,  &gSettings.cameraAELevel,    cycleRangeM2P2,  fmtSignedInt,   applyExposure   },
  { "Sharpness",  CV_INT,  &gSettings.cameraSharpness,  cycleRangeM2P2,  fmtSignedInt,   applySharpness  },
  { "Denoise",    CV_INT,  &gSettings.cameraDenoise,    cycleDenoise,    fmtUnsignedInt, applyDenoise    },
  { "H Mirror",   CV_BOOL, &gSettings.cameraHMirror,    cycleBool,       fmtBool,        applyHMirror    },
  { "V Flip",     CV_BOOL, &gSettings.cameraVFlip,      cycleBool,       fmtBool,        applyVFlip      },
  { "Quality",    CV_INT,  &gSettings.cameraQuality,    cycleQuality,    fmtUnsignedInt, applyQuality    },
};
static const size_t kSettingsCount = sizeof(kSettings) / sizeof(kSettings[0]);

// -----------------------------------------------------------------------------
// Row buffer
// -----------------------------------------------------------------------------

#define CAM_SETTINGS_ROW_LEN  32
// 1 back row + N settings rows. The picker page needs 1 back row + 11
// resolution rows = 12, so size the shared buffer for that.
static char        gRows[1 + 16][CAM_SETTINGS_ROW_LEN];
static const char* gRowPtrs[1 + 16];

// -----------------------------------------------------------------------------
// Render — main settings list
// -----------------------------------------------------------------------------

static size_t buildRows() {
  size_t row = 0;
  strncpy(gRows[row], "<- Camera", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  for (size_t i = 0; i < kSettingsCount && row < (sizeof(gRows) / sizeof(gRows[0])); i++) {
    const CamSetting& s = kSettings[i];
    char valueBuf[16];
    valueBuf[0] = '\0';
    if (s.format) s.format(valueBuf, sizeof(valueBuf), readSetting(s));
    // Resolution is the only setting with a sub-page; show "Resolution: <val> >"
    // so the user has a visual hint that tapping opens a picker rather
    // than cycling like the other rows.
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
// Render — resolution picker sub-page
// -----------------------------------------------------------------------------

static size_t buildPickerRows() {
  const int current = (int)gSettings.cameraFramesize;

  size_t row = 0;
  strncpy(gRows[row], "<- Settings", CAM_SETTINGS_ROW_LEN - 1);
  gRows[row][CAM_SETTINGS_ROW_LEN - 1] = '\0';
  gRowPtrs[row] = gRows[row];
  row++;

  // Iterate in the canonical small→large order so the picker reads
  // top-down by physical resolution.
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

static void showPicker() {
  gLevel = CAM_LEVEL_RESOLUTION_PICKER;
  size_t n = buildPickerRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    DEBUG_G2F("[G2] Camera settings: resolution picker shown (%u rows)",
              (unsigned)n);
  }
}

// -----------------------------------------------------------------------------
// CLI text — minimal; the page is interactive-only in practice.
// -----------------------------------------------------------------------------

void g2BuildCameraSettingsInfo(char* out, size_t cap) {
  if (!out || cap == 0) return;
  // Reuse the same row builder so CLI dump matches what the lens shows.
  size_t n = buildRows();
  size_t pos = 0;
  for (size_t i = 0; i < n && pos + 1 < cap; i++) {
    int w = snprintf(out + pos, cap - pos, "%s\n", gRowPtrs[i]);
    if (w < 0) break;
    pos += (size_t)w;
  }
  if (pos < cap) out[pos] = '\0';
}

// -----------------------------------------------------------------------------
// Public — show / handle taps
// -----------------------------------------------------------------------------

void g2ShowCameraSettingsMenu() {
  // Always enter at the LIST level — even if the user was previously
  // on the picker; the entry point is "open camera settings", not
  // "resume wherever I last was".
  gLevel = CAM_LEVEL_LIST;
  size_t n = buildRows();
  if (g2ShowListPage(gRowPtrs, n)) {
    g2SetHijackPage(G2_HIJACK_PAGE_CAMERA_SETTINGS);
    DEBUG_G2F("[G2] Camera settings shown (%u rows)", (unsigned)n);
  } else {
    DEBUG_G2F("[G2] Camera settings show FAILED");
  }
}

static void handleListTap(uint32_t idx) {
  if (idx == 0) {
    // Back to the CAM detail page (NOT all the way to the sensors
    // landing list). g2ReshowSensorsDetail re-renders using the
    // cached gSensorsDetailIdx and falls back to the landing list
    // only if that index has become stale.
    g2ReshowSensorsDetail();
    return;
  }
  const size_t i = (size_t)idx - 1;
  if (i >= kSettingsCount) {
    DEBUG_G2F("[G2] Camera settings: tap idx=%u out of range (count=%u)",
              (unsigned)idx, (unsigned)kSettingsCount);
    return;
  }
  const CamSetting& s = kSettings[i];
  if (!s.valuePtr || !s.cycle || !s.apply) return;

  // Resolution doesn't cycle on tap any more — it opens a picker
  // sub-page so the user can scan the full list and pick directly.
  if (strcmp(s.label, "Resolution") == 0) {
    showPicker();
    return;
  }

  const int prev = readSetting(s);
  const int next = s.cycle(prev);
  writeSetting(s, next);              // local update — apply hook persists too,
                                      // but we update first so re-render reads
                                      // the new value even if apply is a no-op
                                      // (e.g. camera disabled).
  s.apply(next);                      // persist + live-apply via cmd_camera*

  char dispBuf[16] = {0};
  if (s.format) s.format(dispBuf, sizeof(dispBuf), next);
  BROADCAST_PRINTF("[G2] Camera settings: %s %d -> %s",
                   s.label ? s.label : "?", prev, dispBuf);

  // Re-render so the row reflects the new value. Building rows pulls
  // from gSettings directly so this is a single-pass refresh.
  size_t n = buildRows();
  g2ShowListPage(gRowPtrs, n);
}

static void handlePickerTap(uint32_t idx) {
  if (idx == 0) {
    // Back to the main settings list without changing anything.
    gLevel = CAM_LEVEL_LIST;
    size_t n = buildRows();
    g2ShowListPage(gRowPtrs, n);
    return;
  }
  const size_t i = (size_t)idx - 1;
  if (i >= kFramesizeCycleCount) {
    DEBUG_G2F("[G2] Camera settings: picker tap idx=%u out of range (count=%u)",
              (unsigned)idx, (unsigned)kFramesizeCycleCount);
    return;
  }
  const int newSetting = kFramesizeCycleOrder[i];
  const int prevSetting = (int)gSettings.cameraFramesize;
  if (newSetting == prevSetting) {
    BROADCAST_PRINTF("[G2] Camera settings: resolution unchanged (%s)",
                     framesizeFullName(newSetting));
  } else {
    // applyFramesize → cmd_cameraframesize → setSetting + camera restart.
    BROADCAST_PRINTF("[G2] Camera settings: resolution %s -> %s",
                     framesizeFullName(prevSetting),
                     framesizeFullName(newSetting));
    applyFramesize(newSetting);
  }
  // Return to the main settings list so the user sees the new value
  // reflected on the Resolution row.
  gLevel = CAM_LEVEL_LIST;
  size_t n = buildRows();
  g2ShowListPage(gRowPtrs, n);
}

void g2CameraSettingsHandleTap(uint32_t idx) {
  switch (gLevel) {
    case CAM_LEVEL_LIST:              handleListTap(idx);   return;
    case CAM_LEVEL_RESOLUTION_PICKER: handlePickerTap(idx); return;
  }
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_CAMERA_SENSOR
