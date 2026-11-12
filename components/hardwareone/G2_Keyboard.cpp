// =============================================================================
// G2 glasses: arrow-pad keyboard and the keyboard chunking test
// =============================================================================
// Part of the glasses client (G2_Glasses.cpp). The shared state and the core
// functions this file uses are declared in G2_GlassesInternal.h.

#include "G2_Glasses.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "BLE_Server.h"
#include "System_MemUtil.h"
#include "System_Dictation.h"
#include "G2_HijackCmd.h"

#include "G2_HijackFsm.h"
#include "G2_GlassesInternal.h"

// ─────────────────────────────────────────────────────────────────────
// Arrow-pad keyboard — QWERTY grid rendered as four horizontal IMAGE children
// (with the original one-image surface as an automatic fallback), driven by
// a 7-row static nav list. The map-page pattern applied to text entry:
//   list "app" (left)  : X Cancel / Done / Up / Left / Right / Down / Mic / Keys
//   text "kbBuf" (97)  : prompt + live buffer + count (G2_Page_TextEntry's
//                        pushBuffer targets this name/id unchanged)
//   images "kbd0".."kbd3": four full-width bands covering the 288x144
//                        4-bpp key grid, cursor cell inverted.
// Repeated nav taps keep the list intact (no REBUILD → the native highlight
// stays on the tapped row) and only repush the grid image. Shift/symbols
// are grid CELLS, so the full charset needs zero list rebuilds — the old
// keyboard's 6-group cycling disappears entirely.
//
// Ownership/threading: g2KbdPadBegin runs synchronously in the caller's
// context (tap dispatcher, or the map worker's Search flow — the same
// contexts that run showMapCompound-style teardown+CREATE+ack today).
// g2KbdPadHandleTap runs on g2_taps via the "app" ListEvent branch →
// handleHijackMenuTap → text-entry interception, exactly like the legacy
// keyboard. Image pushes block for acks there; acks pump on the BLE tasks,
// so no deadlock. G2_Page_TextEntry stays the single owner of the buffer,
// callbacks and secret policy; this module only owns cursor + pixels.

namespace {  // keyboard pad internals

// 3x5 glyphs for keycap labels (same encoding as G2_Health's tile font;
// duplicated locally because that table is file-static and covers only the
// health tile's subset). '#' = lit. Returns nullptr for unknown chars.
static const char* kbdGlyph(char c) {
  switch (c) {
    case '0': return "###" "#.#" "#.#" "#.#" "###";
    case '1': return ".#." "##." ".#." ".#." "###";
    case '2': return "###" "..#" "###" "#.." "###";
    case '3': return "###" "..#" "###" "..#" "###";
    case '4': return "#.#" "#.#" "###" "..#" "..#";
    case '5': return "###" "#.." "###" "..#" "###";
    case '6': return "###" "#.." "###" "#.#" "###";
    case '7': return "###" "..#" "..#" "..#" "..#";
    case '8': return "###" "#.#" "###" "#.#" "###";
    case '9': return "###" "#.#" "###" "..#" "###";
    case 'A': return ".#." "#.#" "###" "#.#" "#.#";
    case 'B': return "##." "#.#" "##." "#.#" "##.";
    case 'C': return "###" "#.." "#.." "#.." "###";
    case 'D': return "##." "#.#" "#.#" "#.#" "##.";
    case 'E': return "###" "#.." "##." "#.." "###";
    case 'F': return "###" "#.." "##." "#.." "#..";
    case 'G': return "###" "#.." "#.#" "#.#" "###";
    case 'H': return "#.#" "#.#" "###" "#.#" "#.#";
    case 'I': return "###" ".#." ".#." ".#." "###";
    case 'J': return "..#" "..#" "..#" "#.#" ".#.";
    case 'K': return "#.#" "#.#" "##." "#.#" "#.#";
    case 'L': return "#.." "#.." "#.." "#.." "###";
    case 'M': return "#.#" "###" "###" "#.#" "#.#";
    case 'N': return "#.#" "##." "###" "#.#" "#.#";
    case 'O': return "###" "#.#" "#.#" "#.#" "###";
    case 'P': return "###" "#.#" "###" "#.." "#..";
    case 'Q': return "###" "#.#" "#.#" "###" "..#";
    case 'R': return "##." "#.#" "##." "#.#" "#.#";
    case 'S': return "###" "#.." "###" "..#" "###";
    case 'T': return "###" ".#." ".#." ".#." ".#.";
    case 'U': return "#.#" "#.#" "#.#" "#.#" "###";
    case 'V': return "#.#" "#.#" "#.#" "#.#" ".#.";
    case 'W': return "#.#" "#.#" "###" "###" "#.#";
    case 'X': return "#.#" "#.#" ".#." "#.#" "#.#";
    case 'Y': return "#.#" "#.#" ".#." ".#." ".#.";
    case 'Z': return "###" "..#" ".#." "#.." "###";
    case 'a': return "..." "###" "#.#" "#.#" "###";
    case 'b': return "#.." "#.." "###" "#.#" "###";
    case 'c': return "..." "###" "#.." "#.." "###";
    case 'd': return "..#" "..#" "###" "#.#" "###";
    case 'e': return "..." "###" "###" "#.." "###";
    case 'f': return ".##" ".#." "###" ".#." ".#.";
    case 'g': return "..." "###" "#.#" "###" "..#";
    case 'h': return "#.." "#.." "###" "#.#" "#.#";
    case 'i': return ".#." "..." ".#." ".#." ".#.";
    case 'j': return "..#" "..." "..#" "#.#" ".#.";
    case 'k': return "#.." "#.#" "##." "#.#" "#.#";
    case 'l': return "##." ".#." ".#." ".#." "###";
    case 'm': return "..." "###" "###" "#.#" "#.#";
    case 'n': return "..." "##." "#.#" "#.#" "#.#";
    case 'o': return "..." "###" "#.#" "#.#" "###";
    case 'p': return "..." "##." "#.#" "##." "#..";
    case 'q': return "..." "###" "#.#" "###" "..#";
    case 'r': return "..." "##." "#.." "#.." "#..";
    case 's': return "..." "###" "#.." "###" "###";
    case 't': return ".#." "###" ".#." ".#." ".##";
    case 'u': return "..." "#.#" "#.#" "#.#" "###";
    case 'v': return "..." "#.#" "#.#" "#.#" ".#.";
    case 'w': return "..." "#.#" "#.#" "###" "#.#";
    case 'x': return "..." "#.#" ".#." "#.#" "...";
    case 'y': return "..." "#.#" "###" "..#" "##.";
    case 'z': return "..." "###" ".#." "#.." "###";
    case '!': return ".#." ".#." ".#." "..." ".#.";
    case '@': return "###" "#.#" "###" "#.." "###";
    case '#': return "#.#" "###" "#.#" "###" "#.#";
    case '$': return ".#." "##." ".#." ".##" ".#.";
    case '%': return "#.#" "..#" ".#." "#.." "#.#";
    case '^': return ".#." "#.#" "..." "..." "...";
    case '&': return ".#." "#.#" ".#." "#.#" ".##";
    case '*': return "#.#" ".#." "###" ".#." "#.#";
    case '(': return "..#" ".#." ".#." ".#." "..#";
    case ')': return "#.." ".#." ".#." ".#." "#..";
    case '+': return "..." ".#." "###" ".#." "...";
    case '=': return "..." "###" "..." "###" "...";
    case '?': return "###" "..#" ".##" "..." ".#.";
    case '/': return "..#" ".#." ".#." ".#." "#..";
    case '\\': return "#.." ".#." ".#." ".#." "..#";
    case ':': return "..." ".#." "..." ".#." "...";
    case ';': return "..." ".#." "..." ".#." "#..";
    case '\'': return ".#." ".#." "..." "..." "...";
    case '<': return "..#" ".#." "#.." ".#." "..#";
    case '>': return "#.." ".#." "..#" ".#." "#..";
    case '[': return "##." "#.." "#.." "#.." "##.";
    case ']': return ".##" "..#" "..#" "..#" ".##";
    case '{': return ".##" ".#." "#.." ".#." ".##";
    case '}': return "##." ".#." "..#" ".#." "##.";
    case '|': return ".#." ".#." ".#." ".#." ".#.";
    case '~': return "..." ".#." "#.#" "..." "...";
    case '`': return "#.." ".#." "..." "..." "...";
    case ',': return "..." "..." "..." ".#." "#..";
    case '.': return "..." "..." "..." "..." ".#.";
    case '-': return "..." "..." "###" "..." "...";
    case '_': return "..." "..." "..." "..." "###";
    case ' ': return "..." "..." "..." "..." "...";
    default:  return nullptr;
  }
}

// Grid layout: 3 pages x 5 rows x 10 cols. Special cell codes:
//   \x01 shift toggle   \x02 sym/letters toggle   \x08 backspace
//   \x03 space (span start)   \x00 continuation of the span to its left
// The space bar begins in the same cell on every page. It spans 3 cells on
// letter pages and consumes the remaining 7 cells on the symbol page. NO
// double-quote anywhere — text-entry output must stay safe to
// wrap in a quoted CLI argument (CommandArgs has no escaping).
constexpr int kKbdCols = 10;
constexpr int kKbdRows = 5;
static const char kKbdPageLower[kKbdRows][kKbdCols + 1] = {
  { '1','2','3','4','5','6','7','8','9','0', 0 },
  { 'q','w','e','r','t','y','u','i','o','p', 0 },
  { 'a','s','d','f','g','h','j','k','l','\x08', 0 },
  { '\x01','z','x','c','v','b','n','m',',','.', 0 },
  { '\x02','-','_','\x03','\0','\0','@','!','?','=', 0 },
};
static const char kKbdPageUpper[kKbdRows][kKbdCols + 1] = {
  { '1','2','3','4','5','6','7','8','9','0', 0 },
  { 'Q','W','E','R','T','Y','U','I','O','P', 0 },
  { 'A','S','D','F','G','H','J','K','L','\x08', 0 },
  { '\x01','Z','X','C','V','B','N','M',',','.', 0 },
  { '\x02','-','_','\x03','\0','\0','@','!','?','=', 0 },
};
static const char kKbdPageSym[kKbdRows][kKbdCols + 1] = {
  { '1','2','3','4','5','6','7','8','9','0', 0 },
  { '!','@','#','$','%','^','&','*','(',')', 0 },
  { '+','=','?','/','\\',':',';','\'','`','\x08', 0 },
  { '<','>','[',']','{','}','|','~',',','.', 0 },
  { '\x02','-','_','\x03','\0','\0','\0','\0','\0','\0', 0 },
};

// Pad geometry — production list+text(+image) split (Files/Health/Maps).
static constexpr G2ContainerGeom kKbdListGeom = G2_GEOM_SPLIT_LIST;
static constexpr G2ContainerGeom kKbdBufGeom  = G2_GEOM_SPLIT_RIGHT_TOP;
constexpr int  kKbdImgW = 288;
constexpr int  kKbdImgH = 144;
static constexpr const char* kKbdImgName = "imgKbd";
static constexpr uint32_t    kKbdImgCid  = 98;
static constexpr size_t kKbdShadeBytes = (size_t)kKbdImgW * kKbdImgH;
static constexpr size_t kKbdBmpCap = 14 + 40 + 64 +
    ((size_t)kKbdImgW / 2 + 3) * kKbdImgH + 64;

struct KbdPadBand {
  uint16_t y;
  uint16_t h;
  uint32_t cid;
  const char* name;
  uint32_t pushMagic;
};

// Full-width seams sit on blank scanlines between key rows. Rows 1+2 share
// the taller band: Q33 measured this page-toggle-weighted layout at 147 ms
// per canonical action versus 246 ms for the original one-tile surface.
// Raw fragment comments are the no-compression ceiling; normal LZ4 traffic is
// one image-layer fragment per band.
static constexpr KbdPadBand kKbdPadBands[4] = {
  {   0, 30, 2, "kbd0", G2_MAGIC_IMAGE_BASE +  1 },  // raw 2 frags
  {  30, 56, 3, "kbd1", G2_MAGIC_IMAGE_BASE +  3 },  // raw 3 frags
  {  86, 28, 4, "kbd2", G2_MAGIC_IMAGE_BASE +  6 },  // raw 2 frags
  { 114, 30, 5, "kbd3", G2_MAGIC_IMAGE_BASE +  8 },  // raw 2 frags
};
static constexpr uint8_t kKbdRowBand[kKbdRows] = { 0, 1, 1, 2, 3 };
static constexpr uint32_t kKbdPushMagic1   = G2_MAGIC_IMAGE_BASE + 33;   // 243..248 raw
static constexpr uint8_t  kKbdPageMic = 3;
static constexpr uint32_t kKbdMicCountdownHoldMs = 600;
static_assert(kKbdPadBands[0].y == 0 &&
              kKbdPadBands[0].y + kKbdPadBands[0].h == kKbdPadBands[1].y &&
              kKbdPadBands[1].y + kKbdPadBands[1].h == kKbdPadBands[2].y &&
              kKbdPadBands[2].y + kKbdPadBands[2].h == kKbdPadBands[3].y &&
              kKbdPadBands[3].y + kKbdPadBands[3].h == kKbdImgH,
              "keyboard bands must exactly cover the 288x144 grid");

// Nav list rows. A single tap on an arrow moves in that direction; the rowless
// ring DOUBLE_CLICK gesture selects the highlighted grid key while this pad
// owns the presentation. Row 0 is our Cancel row (compound pages own every
// row).
static const char* const kKbdNavRows[] = {
  "X Cancel", "Done", "Up", "Left", "Right", "Down", "Mic / Keys",
};
constexpr size_t   kKbdNavRowCount = sizeof(kKbdNavRows) / sizeof(kKbdNavRows[0]);
constexpr uint32_t kKbdRowCancel = 0, kKbdRowDone = 1, kKbdRowUp = 2,
                   kKbdRowLeft = 3, kKbdRowRight = 4, kKbdRowDown = 5,
                   kKbdRowMic = 6;
static_assert(kKbdNavRowCount == 7, "keyboard nav list must stay Select-free");

struct KbdPadState {
  bool     active;
  bool     surfaceLive;
  bool     fourBands;   // preferred/default surface; false after fallback
  char     side;        // 'L' / 'R' — arm the compound was created on
  uint32_t connectionGeneration;
  uint32_t lifecycleEpoch;
  uint8_t  page;        // 0 lower, 1 upper, 2 sym, 3 microphone
  uint8_t  pageBeforeMic;
  int8_t   curR, curC;  // cursor cell (curC always on a span START)
  bool     dictationPermitted;  // false for secrets or missing G2 authority
  TransportSessionEpoch dictationEpoch;
  uint8_t  micCountdown;        // 3..1 while priming, 0 otherwise
  uint32_t micCountdownNextMs;
  bool     micSignatureValid;
  bool     micSessionStarted;
  bool     micRenderedAvailable;
  bool     micRenderedEpochLive;
  bool     micRenderedPreparing;
  bool     micRenderedContinuous;
  DictationState micRenderedState;
  CommandSource  micRenderedOwner;
  char     micRenderedSource[5];
  char     micRenderedFailure[40];
  bool     gridDirty;   // a push was skipped for backlog — owe one
  bool     paintedValid;
  uint8_t  paintedPage;
  int8_t   paintedR, paintedC;
  uint8_t  dirtyDebt;   // retry the whole attempted mask after partial failure
  char     bufferText[96];  // current text pane for a live one-tile recreate
  uint8_t* shades;      // kKbdImgW*kKbdImgH, 1 B/px, PSRAM
  uint8_t* bmp;         // wrapped 4-bpp BMP, PSRAM
};
EXT_RAM_BSS_ATTR static KbdPadState gKbdPad;   // task-context only (tap dispatcher + begin caller)
static volatile bool     gKbdPadMicPageActive = false;
static volatile bool     gKbdPadMicServicePending = false;
static volatile uint32_t gKbdPadMicServicePendingAtMs = 0;
static volatile uint32_t gKbdPadMicLastServiceMs = 0;
static volatile uint32_t gKbdPadMicPresentationEpoch = 0;

static const char (*kbdPageTable(uint8_t page))[kKbdCols + 1] {
  return page == 2 ? kKbdPageSym : (page == 1 ? kKbdPageUpper : kKbdPageLower);
}

// Span helpers: a cell is part of a span if it is \x03 (start) or \0
// (continuation). Cursor always rests on the span START.
static int kbdSpanStart(const char (*tbl)[kKbdCols + 1], int r, int c) {
  while (c > 0 && tbl[r][c] == '\0') c--;
  return c;
}
static int kbdSpanEnd(const char (*tbl)[kKbdCols + 1], int r, int c) {
  while (c + 1 < kKbdCols && tbl[r][c + 1] == '\0') c++;
  return c;
}

static void kbdShade(uint8_t* shades, int x, int y, uint8_t v) {
  if (x >= 0 && x < kKbdImgW && y >= 0 && y < kKbdImgH)
    shades[(size_t)y * kKbdImgW + x] = v;
}

static void kbdFillRect(uint8_t* shades, int x0, int y0,
                        int w, int h, uint8_t v) {
  for (int y = y0; y < y0 + h; y++)
    for (int x = x0; x < x0 + w; x++) kbdShade(shades, x, y, v);
}

static void kbdRect(uint8_t* shades, int x0, int y0,
                    int w, int h, uint8_t v) {
  for (int x = x0; x < x0 + w; x++) {
    kbdShade(shades, x, y0, v);
    kbdShade(shades, x, y0 + h - 1, v);
  }
  for (int y = y0; y < y0 + h; y++) {
    kbdShade(shades, x0, y, v);
    kbdShade(shades, x0 + w - 1, y, v);
  }
}

static void kbdDrawGlyph(uint8_t* shades, char c, int x, int y,
                         uint8_t shade, int scale) {
  const char* g = kbdGlyph(c);
  if (!g) return;
  for (int row = 0; row < 5; row++)
    for (int col = 0; col < 3; col++) {
      if (g[row * 3 + col] != '#') continue;
      kbdFillRect(shades, x + col * scale, y + row * scale,
                  scale, scale, shade);
    }
}

static int kbdTextWidth(const char* text, int scale) {
  if (!text || !text[0] || scale <= 0) return 0;
  return (int)strlen(text) * 4 * scale - scale;
}

static void kbdDrawText(uint8_t* shades, const char* text, int x, int y,
                        uint8_t shade, int scale) {
  if (!text || scale <= 0) return;
  for (const char* p = text; *p; ++p, x += 4 * scale) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    kbdDrawGlyph(shades, c, x, y, shade, scale);
  }
}

static void kbdDrawCenteredText(uint8_t* shades, const char* text, int y,
                                uint8_t shade, int scale) {
  const int x = (kKbdImgW - kbdTextWidth(text, scale)) / 2;
  kbdDrawText(shades, text, x, y, shade, scale);
}

// Dedicated speech-input page. It deliberately updates only on state changes;
// a live VU animation would compete with the G2 microphone stream for the same
// BLE link and make the very recording it visualizes less reliable.
static void kbdRenderMicShades(uint8_t* shades) {
  memset(shades, 0, (size_t)kKbdImgW * kKbdImgH);
  kbdDrawCenteredText(shades, "MIC", 8, 15, 5);

  if (gKbdPad.micCountdown > 0) {
    char digit[2] = { (char)('0' + gKbdPad.micCountdown), '\0' };
    // Entire numeral stays inside band 1 (y=30..85), so 3→2→1 updates send
    // one child instead of repainting all four keyboard bands.
    kbdDrawCenteredText(shades, digit, 48, 15, 7);
    kbdDrawCenteredText(shades, "GET READY", 101, 10, 2);
    kbdDrawCenteredText(shades, "LIST ROW STAYS FOCUSED", 126, 6, 1);
    return;
  }

  const DictationSnapshot snap = dictationSnapshotNow();
  char source[20];
  snprintf(source, sizeof(source), "SOURCE %s",
           snap.sourceName ? snap.sourceName : "none");
  kbdDrawCenteredText(shades, source, 39, 8, 2);

  const bool epochLive = gKbdPad.dictationEpoch != kNoTransportSessionEpoch &&
      transportSessionEpochIsLive(SOURCE_G2_GLASSES,
                                  gKbdPad.dictationEpoch);
  const char* unavailable = nullptr;
  const bool available = dictationAvailable(&unavailable);
  const bool ownedElsewhere =
      snap.ownerSource != SOURCE_G2_GLASSES &&
      (snap.state == DictationState::RECORDING ||
       snap.state == DictationState::WAITING);

  const char* line1 = "MIC READY";
  const char* line2 = "TAP MIC TO RETURN";
  const char* footer = "TAP MIC TO RETURN";
  if (!gKbdPad.dictationPermitted) {
    line1 = "MIC DISABLED";
    line2 = "SECRET OR NO USER";
  } else if (!epochLive) {
    line1 = "SESSION GONE";
    line2 = "EXIT KEYBOARD";
  } else if (g2TextEntryPadRemaining() == 0) {
    line1 = "TEXT FIELD FULL";
    line2 = "TAP MIC TO RETURN";
  } else if (ownedElsewhere) {
    line1 = "MIC BUSY";
    line2 = "TAP MIC TO RETURN";
  } else if (!available && snap.state == DictationState::IDLE) {
    line1 = "UNAVAILABLE";
    line2 = unavailable ? unavailable : "NO AUDIO";
  } else if (snap.ownerSource == SOURCE_G2_GLASSES) {
    switch (snap.state) {
      case DictationState::RECORDING:
        line1 = snap.preparing ? "GET READY" : "SPEAK NOW";
        line2 = snap.preparing ? "STARTING MICROPHONE"
                : snap.continuous ? "TAP MIC TO FINISH"
                                     : "STOP SPEAKING TO END TRANSMISSION";
        footer = snap.continuous ? "LISTENS BETWEEN PHRASES"
                                   : "AUTO-STOPS AFTER SILENCE";
        break;
      case DictationState::WAITING:
        line1 = snap.continuous ? "FINISHING" : "TRANSCRIBING";
        line2 = "TAP MIC TO CANCEL";
        footer = "LIST ROW STAYS FOCUSED";
        break;
      case DictationState::FAILED:
        line1 = "FAILED";
        line2 = snap.failure[0] ? snap.failure : "UNKNOWN";
        break;
      case DictationState::IDLE:
      default:
        break;
    }
  }
  kbdDrawCenteredText(shades, line1, 69, 15, 3);
  char line2Fit[35];
  snprintf(line2Fit, sizeof(line2Fit), "%.34s", line2 ? line2 : "");
  kbdDrawCenteredText(shades, line2Fit, 101, 10, 2);
  kbdDrawCenteredText(shades, footer, 126, 6, 1);
}

// Render one keyboard state into a caller-owned 288x144 shade buffer. Keeping
// the renderer state-free lets the on-lens A/B test compare its four bands
// against the exact production pixels without carrying a second renderer.
static void kbdRenderShades(uint8_t* shades, uint8_t page,
                            int8_t curR, int8_t curC) {
  if (!shades) return;
  if (page == kKbdPageMic) {
    kbdRenderMicShades(shades);
    return;
  }
  constexpr int cellW = 28, cellH = 28;
  constexpr int xOff = (kKbdImgW - cellW * kKbdCols) / 2;   // 4
  constexpr int yOff = (kKbdImgH - cellH * kKbdRows) / 2;   // 2
  const char (*tbl)[kKbdCols + 1] = kbdPageTable(page);
  memset(shades, 0, (size_t)kKbdImgW * kKbdImgH);
  for (int r = 0; r < kKbdRows; r++) {
    for (int c = 0; c < kKbdCols; c++) {
      const char code = tbl[r][c];
      if (code == '\0') continue;                 // span continuation
      const int spanEnd = (code == '\x03') ? kbdSpanEnd(tbl, r, c) : c;
      const int x = xOff + c * cellW;
      const int y = yOff + r * cellH;
      const int w = cellW * (spanEnd - c + 1);
      const bool cur = (r == curR && c == curC);
      // Shift key stays visually latched while UPPER is active.
      const bool latched = (code == '\x01' && page == 1);
      if (cur) {
        kbdFillRect(shades, x + 1, y + 1, w - 2, cellH - 2, 15);
      } else {
        kbdRect(shades, x + 1, y + 1, w - 2, cellH - 2,
                latched ? 15 : 6);
      }
      const uint8_t ink = cur ? 0 : 15;
      // Label: centered 3x5 glyph at scale 3 (9x15). Special cells get
      // bespoke marks.
      const int gx = x + (w - 9) / 2;
      const int gy = y + (cellH - 15) / 2;
      switch (code) {
        case '\x01': {  // shift: up arrow
          for (int i = 0; i < 5; i++)
            kbdFillRect(shades, gx + 4 - i, gy + 3 + i * 2,
                        1 + i * 2, 2, ink);
          break;
        }
        case '\x02': {  // page toggle: "#!" on letter pages, "ab" on sym
          const char l0 = (page == 2) ? 'a' : '#';
          const char l1 = (page == 2) ? 'b' : '!';
          kbdDrawGlyph(shades, l0, x + (w - 15) / 2,     gy + 2, ink, 2);
          kbdDrawGlyph(shades, l1, x + (w - 15) / 2 + 9, gy + 2, ink, 2);
          break;
        }
        case '\x08': {  // backspace: left arrow
          for (int i = 0; i < 5; i++)
            kbdFillRect(shades, gx + 3 + i * 2, gy + 4 - i,
                        2, 1 + i * 2, ink);
          break;
        }
        case '\x03':    // space bar: empty wide key, no label
          break;
        default:
          kbdDrawGlyph(shades, code, gx, gy, ink, 3);
          break;
      }
    }
  }
}

static G2Temple* kbdPadArm() {
  G2Temple* arm = (gKbdPad.side == 'L') ? &gL : &gR;
  if (!arm->connected || arm->pluginDead ||
      !gKbdPad.connectionGeneration ||
      arm->connectionGeneration != gKbdPad.connectionGeneration) return nullptr;
  return arm;
}

struct KbdPadCursor {
  uint8_t page;
  int8_t r;
  int8_t c;
};

static KbdPadCursor kbdPadCurrentCursor() {
  return { gKbdPad.page, gKbdPad.curR, gKbdPad.curC };
}

// Exact pixel-change oracle for endpoint coalescing. The cursor contributes
// its old and new row bands. Page changes contribute every band whose table
// character changes, plus the shift latch and symbols-toggle glyphs whose
// pixels change even though their control byte does not. Exhaustive host
// validation covers all 19,600 pairs of the 140 valid keyboard states.
static uint8_t kbdPadEndpointMask(const KbdPadCursor& before,
                                  const KbdPadCursor& after) {
  uint8_t mask = 0;
  // The mic page replaces the whole key grid with status text. Every boundary
  // crossing therefore dirties all four bands; the key-page oracle below is
  // intentionally limited to its three character tables.
  if (before.page != after.page &&
      (before.page == kKbdPageMic || after.page == kKbdPageMic)) {
    return 0x0f;
  }
  if (before.page != after.page) {
    const char (*oldTbl)[kKbdCols + 1] = kbdPageTable(before.page);
    const char (*newTbl)[kKbdCols + 1] = kbdPageTable(after.page);
    const bool shiftLatchFlips = (before.page == 1) != (after.page == 1);
    const bool symLabelFlips = (before.page == 2) != (after.page == 2);
    for (int r = 0; r < kKbdRows; r++) {
      for (int c = 0; c < kKbdCols; c++) {
        const char oldCode = oldTbl[r][c];
        const char newCode = newTbl[r][c];
        if (oldCode != newCode ||
            (shiftLatchFlips &&
             (oldCode == '\x01' || newCode == '\x01')) ||
            (symLabelFlips &&
             (oldCode == '\x02' || newCode == '\x02'))) {
          mask |= (uint8_t)(1u << kKbdRowBand[r]);
          break;
        }
      }
    }
  }
  if (before.r != after.r || before.c != after.c) {
    mask |= (uint8_t)(1u << kKbdRowBand[before.r]);
    mask |= (uint8_t)(1u << kKbdRowBand[after.r]);
  }
  return mask;
}

static bool kbdPadSurfaceEligible() {
  return kbdPadArm() && g2LensGetState().hijackActive && !gImgProbeAbort;
}

static void kbdPadBuildTiles(bool fourBands, G2ImageTile* out,
                             size_t* outCount) {
  if (!out || !outCount) return;
  if (!fourBands) {
    out[0] = { 280, 128, (uint32_t)kKbdImgW, (uint32_t)kKbdImgH,
               kKbdImgCid, kKbdImgName };
    *outCount = 1;
    return;
  }
  for (size_t i = 0; i < 4; i++) {
    out[i] = { 280, (uint32_t)(128 + kKbdPadBands[i].y),
               (uint32_t)kKbdImgW, (uint32_t)kKbdPadBands[i].h,
               kKbdPadBands[i].cid, kKbdPadBands[i].name };
  }
  *outCount = 4;
}

static bool kbdPadCreateSurface(bool fourBands) {
  G2Temple* arm = kbdPadArm();
  if (!arm || !kbdPadSurfaceEligible()) return false;

  const char* navRows[kKbdNavRowCount];
  memcpy(navRows, kKbdNavRows, sizeof(navRows));
  if (!gKbdPad.dictationPermitted) navRows[kKbdRowMic] = "Mic disabled";

  G2ImageTile tiles[4] = {};
  size_t tileCount = 0;
  kbdPadBuildTiles(fourBands, tiles, &tileCount);
  G2TextChildSpec buf = {};
  buf.containerName = "kbBuf";
  buf.content       = gKbdPad.bufferText;
  buf.containerId   = 97;
  buf.geom          = kKbdBufGeom;
  buf.eventCapture  = false;

  const uint32_t magic = allocCreateMagic();
  uint8_t* pb = (uint8_t*)ps_alloc(1400, AllocPref::PreferPsram,
                                   "g2.pb.kbd");
  if (!pb) return false;
  const size_t pbLen = g2BuildCreateMixedListTextImagesPb(
      magic, "app", navRows, kKbdNavRowCount, kKbdListGeom,
      buf, tiles, tileCount, BLOCKS_WIDGET_ID,
      G2_LTI_ORDER_LIST_TEXT_IMAGE, pb, 1400);
  if (!pbLen) {
    free(pb);
    return false;
  }

  if (!probePrepImageCreateAck(magic, arm,
                               gKbdPad.connectionGeneration)) {
    free(pb);
    return false;
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*arm, &createFence)) {
    g2CreateTxnRelease();
    free(pb);
    return false;
  }
  if (!kbdPadSurfaceEligible()) {
    g2CreateTxnRelease();
    free(pb);
    return false;
  }
  const uint32_t started = millis();
  const bool sent = g2SendCreateTxnPb(*arm, pb, pbLen);
  free(pb);
  if (!sent) g2CreateTxnRelease();
  const bool acked = sent && probeWaitImageCreateAck(
      magic, kImgCreateAckTimeoutMs, arm, createFence.generation,
      /*retainForPublication*/ true);
  DEBUG_G2F("[G2] kbd-pad: CREATE mode=%s pb=%uB ack=%u ms=%u",
            fourBands ? "4-band" : "1-tile", (unsigned)pbLen,
            acked ? 1u : 0u, (unsigned)(millis() - started));
  if (!acked) return false;
  if (!kbdPadSurfaceEligible()) {
    g2CompensateRejectedCreate(*arm, "kbd surface revoked");
    g2CreateTxnRelease();
    return false;
  }

  if (!g2NoteCreateSuccess(*arm, /*isList*/ true, BLOCKS_WIDGET_ID,
                           createFence.lifecycleEpoch)) {
    return false;
  }
  gKbdPad.connectionGeneration = createFence.generation;
  gKbdPad.lifecycleEpoch = createFence.lifecycleEpoch;
  // g2NoteCreateSuccess advances the global presentation epoch, which
  // intentionally revokes every old gesture claim. Reclaim this exact new
  // keyboard surface in transition mode until its initial pixels land.
  kbdPadSysDoubleClaim(gKbdPad.side);
  tapLabelCacheStore(navRows, kKbdNavRowCount);
  gLastHijackListRowCount  = kKbdNavRowCount;
  gLastHijackListPageShape = HijackListPageShape::ListWithTitle;
  g2TextViewDisarm();
  gKbdPad.fourBands = fourBands;
  gKbdPad.surfaceLive = true;
  gKbdPad.paintedValid = false;
  gKbdPad.dirtyDebt = 0;
  return true;
}

// Push the current logical state into the live surface. The dirty mask is
// calculated from the last fully acknowledged endpoint, not by OR-ing every
// intermediate move in a burst. If a multi-band push fails after some children
// landed, the whole attempted mask becomes debt so no hybrid frame is trusted.
static bool kbdPadPushRendered(const char* why, bool forceFull,
                               uint8_t requestedMask = 0) {
  G2Temple* arm = kbdPadArm();
  if (!arm || !gKbdPad.surfaceLive || !kbdPadSurfaceEligible()) return false;
  const KbdPadCursor current = kbdPadCurrentCursor();
  uint8_t mask = 0;
  if (forceFull || !gKbdPad.paintedValid) {
    mask = gKbdPad.fourBands ? 0x0f : 0x01;
  } else if (requestedMask) {
    mask = gKbdPad.fourBands ? (requestedMask & 0x0f) : 0x01;
    mask |= gKbdPad.dirtyDebt;
  } else {
    const KbdPadCursor painted = {
      gKbdPad.paintedPage, gKbdPad.paintedR, gKbdPad.paintedC
    };
    const uint8_t endpoint = kbdPadEndpointMask(painted, current);
    mask = gKbdPad.fourBands ? endpoint : (endpoint ? 0x01 : 0x00);
    mask |= gKbdPad.dirtyDebt;
  }
  if (!mask) {
    gKbdPad.paintedPage = current.page;
    gKbdPad.paintedR = current.r;
    gKbdPad.paintedC = current.c;
    gKbdPad.paintedValid = true;
    return true;
  }

  kbdRenderShades(gKbdPad.shades, current.page, current.r, current.c);
  unsigned pushedTiles = 0;
  unsigned imageFrags = 0;
  size_t wireBytes = 0;
  bool ok = true;
  const size_t tileCount = gKbdPad.fourBands ? 4 : 1;
  for (size_t i = 0; i < tileCount && ok; i++) {
    if ((mask & (1u << i)) == 0) continue;
    const uint16_t y = gKbdPad.fourBands ? kKbdPadBands[i].y : 0;
    const uint16_t h = gKbdPad.fourBands ? kKbdPadBands[i].h : kKbdImgH;
    const uint32_t cid = gKbdPad.fourBands
        ? kKbdPadBands[i].cid : kKbdImgCid;
    const char* name = gKbdPad.fourBands
        ? kKbdPadBands[i].name : kKbdImgName;
    const uint32_t magic = gKbdPad.fourBands
        ? kKbdPadBands[i].pushMagic : kKbdPushMagic1;
    const size_t bmpLen = g2BuildBmp4bppFromShades(
        gKbdPad.bmp, kKbdBmpCap,
        gKbdPad.shades + (size_t)y * kKbdImgW, kKbdImgW, h);
    if (!bmpLen) { ok = false; break; }

    unsigned okFrags = 0, totalFrags = 0;
    size_t thisWireBytes = 0;
    char tag[20];
    snprintf(tag, sizeof(tag), "kbdPad/%s%u",
             gKbdPad.fourBands ? "b" : "full", (unsigned)i);
    ok = sendImageBmpFragmentsNoCreate(
        *arm, tag, magic, cid, name, gKbdPad.bmp, bmpLen,
        &okFrags, &totalFrags, /*tolerateMissedAcks*/ 0,
        /*inFlightCapOverride*/ 0, G2_IMG_COMPRESS_RAW,
        &thisWireBytes, gKbdPad.connectionGeneration,
        /*markExpectedEcho*/ false, gKbdPad.lifecycleEpoch);
    pushedTiles++;
    imageFrags += totalFrags;
    wireBytes += thisWireBytes;
  }

  if (!ok) {
    gKbdPad.dirtyDebt |= mask;
  } else {
    gKbdPad.paintedPage = current.page;
    gKbdPad.paintedR = current.r;
    gKbdPad.paintedC = current.c;
    gKbdPad.paintedValid = true;
    gKbdPad.dirtyDebt = 0;
  }
  DEBUG_G2F("[G2] kbd-pad: mode=%s mask=0x%02x tiles=%u "
            "wire=%u imgfrags=%u ok=%u (%s)",
            gKbdPad.fourBands ? "4-band" : "1-tile", (unsigned)mask,
            pushedTiles, (unsigned)wireBytes, imageFrags, ok ? 1u : 0u,
            why ? why : "?");
  return ok;
}

static bool kbdPadForceShutdown() {
  if (kbdPadSysDoubleOwned()) {
    kbdPadSysDoubleTransition();
  }
  G2Temple* arm = kbdPadArm();
  if (!arm) return false;
  const bool ok = tearDownActiveContainerAtGeneration(
      *arm, gKbdPad.connectionGeneration, /*force*/ true);
  if (ok) {
    gKbdPad.surfaceLive = false;
    gKbdPad.paintedValid = false;
    gKbdPad.dirtyDebt = 0;
  }
  return ok;
}

static bool kbdPadStartOneTileFallback(const char* reason) {
  if (!kbdPadForceShutdown() || !kbdPadSurfaceEligible()) return false;
  // Container clear advances the presentation epoch and revokes the prior
  // claim. Own the recreate interval so a double cannot fall through to a
  // generic back/refresh handler while the new surface is being built.
  kbdPadSysDoubleClaim(gKbdPad.side);
  if (!kbdPadCreateSurface(/*fourBands*/ false) ||
      !kbdPadPushRendered("fallback-initial", /*forceFull*/ true)) {
    (void)kbdPadForceShutdown();
    return false;
  }
  if (gKbdPad.active) {
    kbdPadSysDoublePublish(g2PresentationEpochCurrent());
  }
  DEBUG_G2F("[G2] kbd-pad: four-band fallback -> one tile (%s)",
            reason ? reason : "failure");
  return true;
}

// True when more taps are already queued behind the one being handled —
// the pad skips intermediate grid pushes so a burst of arrow taps costs
// one render+push, not N. The final tap of any burst always pushes.
static bool kbdTapBacklog() {
  QueueHandle_t queue = nullptr;
  if (!tapRuntimeSnapshot(&queue)) return false;
  // tapRuntimeSnapshot INCREMENTS gUiSubmitters on success; the claim must be
  // released or the counter saturates at 0xFF and every future
  // tapDispatcherEnqueue* is refused (all hijack taps dead until reboot), and
  // g2ClientInitIdle() never reports quiescent so BLE teardown stalls.
  G2UiSubmitClaim submitClaim;
  return uxQueueMessagesWaiting(queue) > 0;
}

// Coalescing push: skip while more taps are already queued (a burst of arrow
// taps costs one render+push, not N), but REMEMBER the debt. Callers that
// don't move the cursor (character selection) still settle it, so the lens
// can never keep rendering a stale cursor/page while a double-tap types
// something else.
static bool kbdPushGrid(const char* why);
static bool kbdPushGridForce(const char* why);
static bool kbdPushGridMask(const char* why, uint8_t mask);
static bool kbdPushGridCoalesced(const char* why) {
  if (kbdTapBacklog()) { gKbdPad.gridDirty = true; return true; }
  gKbdPad.gridDirty = false;
  return kbdPushGrid(why);
}
// Settle a deferred push once the burst has drained. Called on every handled
// pad tap, including ones that don't themselves change the grid.
static bool kbdSettleGrid() {
  if (!gKbdPad.gridDirty) return true;
  if (kbdTapBacklog()) return true;     // still bursting — settle later
  gKbdPad.gridDirty = false;
  return kbdPushGrid("settle");
}

static bool kbdPushGridImpl(const char* why, bool forceFull,
                            uint8_t requestedMask = 0) {
  if (kbdPadPushRendered(why, forceFull, requestedMask)) return true;
  if (!gKbdPad.fourBands) {
    // The compatibility surface is still live. Keep the exact dirty debt and
    // retry on the next tap rather than destroying text entry over one missed
    // image ACK.
    DEBUG_G2F("[G2] kbd-pad: one-tile push failed; retaining retry debt");
    return kbdPadSurfaceEligible();
  }

  // A real exit/fresh MenuStartUp uses the same abort flag. Do not clear that
  // terminal signal by entering a new probe and then recreate the keyboard
  // over the replacement presentation.
  if (!kbdPadSurfaceEligible()) return false;
  imageProbeBegin();
  const bool recovered = kbdPadStartOneTileFallback(
      why ? why : "live four-band push failed");
  clearOurShutdownStamp();
  imageProbeEnd();
  if (!recovered)
    DEBUG_G2F("[G2] kbd-pad: live fallback failed; cancelling text entry");
  return recovered;
}

static bool kbdPushGrid(const char* why) {
  return kbdPushGridImpl(why, /*forceFull*/ false);
}

static bool kbdPushGridForce(const char* why) {
  gKbdPad.gridDirty = false;
  return kbdPushGridImpl(why, /*forceFull*/ true);
}

static bool kbdPushGridMask(const char* why, uint8_t mask) {
  gKbdPad.gridDirty = false;
  return kbdPushGridImpl(why, /*forceFull*/ false, mask);
}

static bool kbdPadMicStatusChanged(bool remember) {
  const DictationSnapshot snap = dictationSnapshotNow();
  const bool available = dictationAvailable(nullptr);
  const bool epochLive = gKbdPad.dictationEpoch != kNoTransportSessionEpoch &&
      transportSessionEpochIsLive(SOURCE_G2_GLASSES,
                                  gKbdPad.dictationEpoch);
  const char* source = snap.sourceName ? snap.sourceName : "none";
  const bool changed = !gKbdPad.micSignatureValid ||
      gKbdPad.micRenderedAvailable != available ||
      gKbdPad.micRenderedEpochLive != epochLive ||
      gKbdPad.micRenderedPreparing != snap.preparing ||
      gKbdPad.micRenderedContinuous != snap.continuous ||
      gKbdPad.micRenderedState != snap.state ||
      gKbdPad.micRenderedOwner != snap.ownerSource ||
      strcmp(gKbdPad.micRenderedSource, source) != 0 ||
      strcmp(gKbdPad.micRenderedFailure, snap.failure) != 0;
  if (remember) {
    gKbdPad.micSignatureValid = true;
    gKbdPad.micRenderedAvailable = available;
    gKbdPad.micRenderedEpochLive = epochLive;
    gKbdPad.micRenderedPreparing = snap.preparing;
    gKbdPad.micRenderedContinuous = snap.continuous;
    gKbdPad.micRenderedState = snap.state;
    gKbdPad.micRenderedOwner = snap.ownerSource;
    snprintf(gKbdPad.micRenderedSource,
             sizeof(gKbdPad.micRenderedSource), "%s", source);
    snprintf(gKbdPad.micRenderedFailure,
             sizeof(gKbdPad.micRenderedFailure), "%s", snap.failure);
  }
  return changed;
}

static void kbdPadMicServiceSuspend() {
  __atomic_store_n(&gKbdPadMicPageActive, false, __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdPadMicServicePending, false, __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdPadMicPresentationEpoch, 0u, __ATOMIC_RELEASE);
}

static void kbdPadMicServiceResume() {
  __atomic_store_n(&gKbdPadMicLastServiceMs, millis(), __ATOMIC_RELAXED);
  __atomic_store_n(&gKbdPadMicPresentationEpoch,
                   g2PresentationEpochCurrent(), __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdPadMicPageActive, true, __ATOMIC_RELEASE);
}

static bool kbdPadReturnToKeys(const char* why, bool cancelDictation) {
  kbdPadMicServiceSuspend();
  if (cancelDictation) dictationCancelFor(SOURCE_G2_GLASSES);
  gKbdPad.micCountdown = 0;
  gKbdPad.micSessionStarted = false;
  gKbdPad.micCountdownNextMs = 0;
  gKbdPad.page = gKbdPad.pageBeforeMic <= 2 ? gKbdPad.pageBeforeMic : 0;
  gKbdPad.micSignatureValid = false;
  return kbdPushGridForce(why);
}

static void kbdPadFreeBuffers() {
  if (gKbdPad.shades) { free(gKbdPad.shades); gKbdPad.shades = nullptr; }
  if (gKbdPad.bmp)    { free(gKbdPad.bmp);    gKbdPad.bmp    = nullptr; }
}

}  // namespace

// Both providers use the same receipt path. The field owns capacity/filtering;
// only source bytes actually consumed by that policy are committed.
static bool kbdPadConsumeDictationText() {
  char transcript[DICTATION_MAX_TEXT + 1] = {};
  DictationTextReceipt receipt;
  if (!dictationPeekTextFor(SOURCE_G2_GLASSES, transcript, sizeof(transcript), &receipt))
    return false;
  bool needsSpace = false;
  const size_t remaining = g2TextEntryPadRemaining(&needsSpace);
  const size_t prefix = receipt.offset == 0 && needsSpace ? 1 : 0;
  if (remaining <= prefix) {
    memset(transcript, 0, sizeof(transcript));
    dictationFieldFullFor(SOURCE_G2_GLASSES);
    return true;
  }
  const size_t amount = std::min(static_cast<size_t>(receipt.length), remaining - prefix);
  char insertion[DICTATION_MAX_TEXT + 2] = {};
  if (prefix) insertion[0] = ' ';
  memcpy(insertion + prefix, transcript, amount);
  size_t consumed = 0;
  (void)g2TextEntryPadAppendText(insertion, &consumed);
  memset(insertion, 0, sizeof(insertion));
  memset(transcript, 0, sizeof(transcript));
  const size_t accepted = consumed >= prefix ? consumed - prefix : 0;
  if (accepted && !dictationCommitTextFor(SOURCE_G2_GLASSES, receipt, accepted)) {
    dictationCancelFor(SOURCE_G2_GLASSES);
    return true;
  }
  if (!g2TextEntryPadRemaining()) dictationFieldFullFor(SOURCE_G2_GLASSES);
  return consumed != 0;
}

// Tap-worker side of the G2 dictation bridge. The main loop services recorder
// timeouts, then queues this exact-presentation entry to drain a transcript and
// repaint status. The pending timestamp self-heals if the tap dispatcher
// rejects an entry as stale during a presentation transition.
void kbdPadDictationServiceOnTap(uint32_t presentationEpoch) {
  __atomic_store_n(&gKbdPadMicServicePending, false, __ATOMIC_RELEASE);
  G2TextEntryOperationGuard operation;
  if (!operation || presentationEpoch == 0 ||
      presentationEpoch != g2PresentationEpochCurrent() ||
      presentationEpoch != __atomic_load_n(
          &gKbdPadMicPresentationEpoch, __ATOMIC_ACQUIRE)) return;
  if (!gKbdPad.active || gKbdPad.page != kKbdPageMic ||
      !g2TextEntryIsActive()) return;

  // Unlike the original ListEvent, countdown/status work re-enters this worker
  // as a dedicated keyboard-service entry after handleHijackMenuTap's identity
  // guard has unwound. Restore the paired G2 user before dictation can create its owned
  // WAV through VFS::openGuarded; otherwise the worker's default ANON context
  // turns a healthy recorder start into a filesystem-denied setup failure.
  G2HijackCtxGuard ctxGuard;

  if (gKbdPad.micCountdown > 0) {
    const uint32_t now = millis();
    if ((int32_t)(now - gKbdPad.micCountdownNextMs) < 0) return;

    kbdPadMicServiceSuspend();
    if (gKbdPad.micCountdown > 1) {
      gKbdPad.micCountdown--;
      if (!kbdPushGridMask("mic-countdown", /*band 1*/ 0x02)) {
        g2TextEntryPadEvent('\x1b');
        return;
      }
      gKbdPad.micCountdownNextMs = millis() + kKbdMicCountdownHoldMs;
      kbdPadMicServiceResume();
      return;
    }

    // "1" has held for its full interval. Only now arm the mic, so the
    // wearer never loses the first word while the countdown is still painting.
    gKbdPad.micCountdown = 0;
    const bool armed = g2TextEntryPadRemaining() > 0 &&
        dictationBeginFor(SOURCE_G2_GLASSES, gKbdPad.dictationEpoch);
    gKbdPad.micSessionStarted = armed;
    DEBUG_G2F("[G2] kbd-pad: countdown complete, arm=%u", armed ? 1u : 0u);
    // Header is unchanged; recording/busy/failure state replaces the digit in
    // band 1, GET READY in band 2, and may change the footer in band 3.
    if (!kbdPushGridMask("mic-countdown-arm", /*bands 1..3*/ 0x0e)) {
      g2TextEntryPadEvent('\x1b');
      return;
    }
    if (gKbdPad.dirtyDebt == 0)
      (void)kbdPadMicStatusChanged(/*remember*/ true);
    else
      gKbdPad.micSignatureValid = false;
    kbdPadMicServiceResume();
    return;
  }

  dictationTick();
  const bool took = kbdPadConsumeDictationText();
  const DictationSnapshot after = dictationSnapshotNow();
  // Both providers finish only when their final result has been consumed.
  // Continuous sessions stay on this page between text chunks.
  if (gKbdPad.micSessionStarted && after.state == DictationState::IDLE) {
    if (!kbdPadReturnToKeys("mic-text-return", /*cancelDictation*/ false))
      g2TextEntryPadEvent('\x1b');
    return;
  }
  if (took) gKbdPad.micSignatureValid = false;

  if (kbdPadMicStatusChanged(/*remember*/ false)) {
    kbdPadMicServiceSuspend();
    if (!kbdPushGridForce("mic-state")) {
      g2TextEntryPadEvent('\x1b');
      return;
    }
    if (gKbdPad.dirtyDebt == 0)
      (void)kbdPadMicStatusChanged(/*remember*/ true);
    else
      gKbdPad.micSignatureValid = false;  // retry on the next service tick
    kbdPadMicServiceResume();
  }
}

void kbdPadDictationTickMain() {
  if (!__atomic_load_n(&gKbdPadMicPageActive, __ATOMIC_ACQUIRE)) return;

  dictationTick();
  const uint32_t micEpoch = __atomic_load_n(
      &gKbdPadMicPresentationEpoch, __ATOMIC_ACQUIRE);
  if (!micEpoch || micEpoch != g2PresentationEpochCurrent() ||
      !g2LensGetState().hijackActive || gImgProbeAbort ||
      !g2TextEntryIsActive()) {
    __atomic_store_n(&gKbdPadMicPageActive, false, __ATOMIC_RELEASE);
    __atomic_store_n(&gKbdPadMicServicePending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&gKbdPadMicPresentationEpoch, 0u, __ATOMIC_RELEASE);
    dictationCancelFor(SOURCE_G2_GLASSES);
    return;
  }

  const uint32_t now = millis();
  const uint32_t last = __atomic_load_n(&gKbdPadMicLastServiceMs,
                                        __ATOMIC_RELAXED);
  if ((uint32_t)(now - last) < 200u) return;

  const bool pending = __atomic_load_n(&gKbdPadMicServicePending,
                                       __ATOMIC_ACQUIRE);
  const uint32_t pendingAt = __atomic_load_n(&gKbdPadMicServicePendingAtMs,
                                             __ATOMIC_RELAXED);
  if (pending && (uint32_t)(now - pendingAt) < 1000u) return;

  __atomic_store_n(&gKbdPadMicLastServiceMs, now, __ATOMIC_RELAXED);
  __atomic_store_n(&gKbdPadMicServicePendingAtMs, now, __ATOMIC_RELAXED);
  __atomic_store_n(&gKbdPadMicServicePending, true, __ATOMIC_RELEASE);
  if (!tapDispatcherEnqueueKeyboardService(micEpoch)) {
    __atomic_store_n(&gKbdPadMicServicePending, false, __ATOMIC_RELEASE);
  }
}

// Terminal/cmd17 lifecycle reset tombstones the exact text-entry identity
// immediately, so routing and callbacks stop synchronously. It deliberately
// does not free these ~63 KB buffers on g2_owner: an exact cleanup token
// is queued behind older work on g2_taps, which calls g2KbdPadEnd only
// after any in-flight push has returned. Queue saturation and a busy session
// worker leave the tombstone pending; g2Tick retries the FIFO wake.

void g2KbdPadEnd() {
  kbdPadSysDoubleUnpublish();
  const bool hadState = gKbdPad.active || gKbdPad.shades || gKbdPad.bmp;
  gKbdPad.active = false;
  __atomic_store_n(&gKbdPadMicPageActive, false, __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdPadMicServicePending, false, __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdPadMicPresentationEpoch, 0u, __ATOMIC_RELEASE);
  dictationCancelFor(SOURCE_G2_GLASSES);
  if (!hadState) return;
  gKbdPad.surfaceLive = false;
  gKbdPad.paintedValid = false;
  memset(gKbdPad.bufferText, 0, sizeof(gKbdPad.bufferText));
  kbdPadFreeBuffers();
  DEBUG_G2F("[G2] kbd-pad: ended");
}

void g2KbdPadNoteBufferText(const char* bufText) {
  if (!gKbdPad.active) return;
  snprintf(gKbdPad.bufferText, sizeof(gKbdPad.bufferText), "%s",
           bufText ? bufText : "");
}

// Create the mixed list+text+image compound and push the initial grid.
// Synchronous —
// same teardown+CREATE+ack sequence the map page runs from these contexts.
// Returns false (with everything freed) so the caller can fall back to the
// legacy list keyboard.
bool g2KbdPadBegin(const char* bufText) {
  kbdPadSysDoubleUnpublish();
  if (gKbdPad.active || gKbdPad.shades || gKbdPad.bmp)
    g2KbdPadEnd();   // replace-session + reclaim abandoned-session buffers
  // Also close a source-owned exchange left behind by an earlier surface that
  // never reached the buffer-allocation stage. This cannot affect OLED-owned
  // dictation because cancellation is source-bound.
  dictationCancelFor(SOURCE_G2_GLASSES);
  __atomic_store_n(&gKbdPadMicPageActive, false, __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdPadMicServicePending, false, __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdPadMicPresentationEpoch, 0u, __ATOMIC_RELEASE);
  if (pageSwapInFlight()) {
    DEBUG_G2F("[G2] kbd-pad: page swap in flight — falling back");
    return false;
  }
  G2Temple* arm = nullptr;
  if (gR.connected && !gR.pluginDead)      arm = &gR;
  else if (gL.connected && !gL.pluginDead) arm = &gL;
  if (!arm) { DEBUG_G2F("[G2] kbd-pad: no eligible temple"); return false; }

  // Clear stale image-probe state so a leftover flag can't hijack the tap
  // stream or abort pushes (same defensive clears as the map page).
  gImgProbeHoldTapPending = false;
  gImgProbeAbort          = false;
  gImgProbeHoldActive     = false;

  memset(&gKbdPad, 0, sizeof(gKbdPad));
  gKbdPad.side = arm->side;
  gKbdPad.connectionGeneration = arm->connectionGeneration;
  gKbdPad.page = 0;
  gKbdPad.pageBeforeMic = 0;
  gKbdPad.curR = 1;      // start on 'q' — top-left of the letters
  gKbdPad.curC = 0;
  const AuthContext dictCtx = g2HijackAuthContext();
  gKbdPad.dictationEpoch = captureTransportSessionEpoch(dictCtx);
  gKbdPad.dictationPermitted = !g2TextEntryIsSecret() &&
      gKbdPad.dictationEpoch != kNoTransportSessionEpoch;
  snprintf(gKbdPad.bufferText, sizeof(gKbdPad.bufferText), "%s",
           bufText ? bufText : "");

  gKbdPad.shades = (uint8_t*)ps_alloc(kKbdShadeBytes,
                                      AllocPref::PreferPsram,
                                      "g2.kbd.shades");
  gKbdPad.bmp    = (uint8_t*)ps_alloc(kKbdBmpCap,
                                      AllocPref::PreferPsram,
                                      "g2.kbd.bmp");
  if (!gKbdPad.shades || !gKbdPad.bmp) {
    DEBUG_G2F("[G2] kbd-pad: buffer alloc failed");
    kbdPadFreeBuffers();
    return false;
  }

  kbdPadSysDoubleClaim(gKbdPad.side);
  imageProbeBegin();
  if (!tearDownActiveContainerAtGeneration(
          *arm, gKbdPad.connectionGeneration, /*force*/ false)) {
    DEBUG_G2F("[G2] kbd-pad: pre-page SHUTDOWN failed");
    clearOurShutdownStamp();
    imageProbeEnd();
    kbdPadFreeBuffers();
    kbdPadSysDoubleUnpublish();
    return false;
  }
  gKbdPad.surfaceLive = false;
  // The successful teardown advanced the presentation epoch and revoked the
  // pre-teardown claim. Reclaim the CREATE interval; kbdPadCreateSurface will
  // replace this transition token after the new CREATE is acknowledged.
  kbdPadSysDoubleClaim(gKbdPad.side);

  // Four bands are the production default. A rejected/ambiguous CREATE or
  // failed initial band push is quarantined with SHUTDOWN before the disjoint
  // one-tile magic bank recreates the exact prior surface.
  bool started = kbdPadCreateSurface(/*fourBands*/ true) &&
                 kbdPadPushRendered("initial", /*forceFull*/ true);
  if (!started && kbdPadSurfaceEligible()) {
    DEBUG_G2F("[G2] kbd-pad: four-band startup failed; trying one tile");
    started = kbdPadStartOneTileFallback("startup create/push failed");
  }
  if (!started) {
    (void)kbdPadForceShutdown();
    clearOurShutdownStamp();
    imageProbeEnd();
    kbdPadFreeBuffers();
    kbdPadSysDoubleUnpublish();
    DEBUG_G2F("[G2] kbd-pad: both image surfaces failed; using legacy list");
    return false;
  }

  // Leave ImageProbing once the initial surface is live. Without this
  // bookend, normal page swaps and real SYSTEM_EXIT/DISPLAY_OFF events remain
  // suppressed for the rest of the text-entry session.
  clearOurShutdownStamp();
  imageProbeEnd();
  gKbdPad.active = true;
  kbdPadSysDoublePublish(g2PresentationEpochCurrent());
  DEBUG_G2F("[G2] kbd-pad: up (default=%s)",
            gKbdPad.fourBands ? "4-band" : "1-tile fallback");
  return true;
}

// Activate the currently highlighted grid cell. Selection is intentionally
// independent of a nav-list index: captured firmware reports the ring gesture
// as a rowless SysEvent DOUBLE_CLICK, while normal ListEvent CLICK keeps moving
// the cursor.
static void kbdPadSelectCurrentKey() {
  if (!gKbdPad.active || gKbdPad.page == kKbdPageMic) return;
  const char (*tbl)[kKbdCols + 1] = kbdPageTable(gKbdPad.page);
  const char code = tbl[gKbdPad.curR][gKbdPad.curC];
  switch (code) {
    case '\x01':   // shift toggle (lower <-> upper)
      gKbdPad.page = (gKbdPad.page == 1) ? 0 : 1;
      if (!kbdPushGridCoalesced("shift"))
        g2TextEntryPadEvent('\x1b');
      return;
    case '\x02':   // sym <-> letters
      gKbdPad.page = (gKbdPad.page == 2) ? 0 : 2;
      // Cursor cell exists identically across pages by construction.
      if (!kbdPushGridCoalesced("sym"))
        g2TextEntryPadEvent('\x1b');
      return;
    // Typing keys patch only the TEXT child (fast path), but must still
    // settle grid debt owed by an earlier skipped cursor/page push — otherwise
    // the lens would highlight one key while the double-tap types another.
    case '\x08':
      g2TextEntryPadEvent('\b');
      if (!kbdSettleGrid()) g2TextEntryPadEvent('\x1b');
      return;
    case '\x03':
      g2TextEntryPadEvent(' ');
      if (!kbdSettleGrid()) g2TextEntryPadEvent('\x1b');
      return;
    default:
      g2TextEntryPadEvent(code);
      if (!kbdSettleGrid()) g2TextEntryPadEvent('\x1b');
      return;
  }
}

// Runs on g2_taps after the SysEvent's presentation epoch and paired-user
// context have been checked. The mic status page remains non-editable.
bool kbdPadHandleDoubleTap() {
  if (!gKbdPad.active || !gKbdPad.surfaceLive ||
      gKbdPad.page == kKbdPageMic || !kbdPadSurfaceEligible()) return false;
  kbdPadSelectCurrentKey();
  return true;
}

// Nav-list tap handler. Runs on g2_taps (via the text-entry
// interception in handleHijackMenuTap) — safe for blocking image pushes.
void g2KbdPadHandleTap(uint32_t idx) {
  if (!gKbdPad.active) return;

  // Cancel/Done retain their meaning on every page, including speech input.
  if (idx == kKbdRowCancel) { g2TextEntryPadEvent('\x1b'); return; }
  if (idx == kKbdRowDone)   { g2TextEntryPadEvent('\n');   return; }

  if (idx == kKbdRowMic) {
    if (gKbdPad.page == kKbdPageMic) {
      const DictationSnapshot snap = dictationSnapshotNow();
      if (snap.ownerSource == SOURCE_G2_GLASSES &&
          snap.state == DictationState::RECORDING) {
        // The focused row doubles as Stop. Do not repaint while the recorder
        // is finalizing; the service tick will switch to TRANSCRIBING once the
        // terminal capture hook publishes WAITING.
        dictationRequestStopFor(SOURCE_G2_GLASSES);
        DEBUG_G2F("[G2] kbd-pad: Mic row requested early stop");
        return;
      }
      // WAITING means cancel transcription; FAILED/IDLE/BUSY simply leave the
      // status page. Source-bound cancellation cannot touch an OLED exchange.
      if (!kbdPadReturnToKeys("mic-cancel-return",
                             /*cancelDictation*/ true))
        g2TextEntryPadEvent('\x1b');
      return;
    }
    if (!gKbdPad.dictationPermitted) {
      DEBUG_G2F("[G2] kbd-pad: mic page refused (secret or no G2 owner)");
      return;
    }
    gKbdPad.pageBeforeMic = gKbdPad.page;
    gKbdPad.page = kKbdPageMic;
    gKbdPad.micCountdown = 3;
    gKbdPad.micSessionStarted = false;
    gKbdPad.micSignatureValid = false;
    kbdPadMicServiceSuspend();
    // The same focused row starts an asynchronous 3→2→1 primer. Recording is
    // armed only after "1" has been visible; the image remains status-only.
    DEBUG_G2F("[G2] kbd-pad: Mic row enter, countdown=3");
    if (!kbdPushGridForce("mic-enter")) {
      g2TextEntryPadEvent('\x1b');
      return;
    }
    if (gKbdPad.dirtyDebt == 0)
      (void)kbdPadMicStatusChanged(/*remember*/ true);
    else
      gKbdPad.micSignatureValid = false;
    gKbdPad.micCountdownNextMs = millis() + kKbdMicCountdownHoldMs;
    kbdPadMicServiceResume();
    return;
  }

  if (gKbdPad.page == kKbdPageMic) {
    return;  // status-only page; all interaction stays on the focused Mic row
  }

  const char (*tbl)[kKbdCols + 1] = kbdPageTable(gKbdPad.page);

  switch (idx) {
    case kKbdRowUp:
    case kKbdRowDown: {
      const int dir = (idx == kKbdRowUp) ? -1 : 1;
      int r = (gKbdPad.curR + dir + kKbdRows) % kKbdRows;
      gKbdPad.curR = (int8_t)r;
      gKbdPad.curC = (int8_t)kbdSpanStart(tbl, r, gKbdPad.curC);
      break;
    }
    case kKbdRowLeft: {
      int c = gKbdPad.curC;
      c = (c == 0) ? kKbdCols - 1 : c - 1;
      c = kbdSpanStart(tbl, gKbdPad.curR, c);
      gKbdPad.curC = (int8_t)c;
      break;
    }
    case kKbdRowRight: {
      int c = kbdSpanEnd(tbl, gKbdPad.curR, gKbdPad.curC) + 1;
      if (c >= kKbdCols) c = 0;
      gKbdPad.curC = (int8_t)c;
      break;
    }
    default: return;   // unmapped row
  }
  if (!kbdPushGridCoalesced("cursor"))
    g2TextEntryPadEvent('\x1b');
}

bool g2KbdPadStartMic() {
  if (!gKbdPad.active || !gKbdPad.surfaceLive || !gKbdPad.dictationPermitted ||
      gKbdPad.page == kKbdPageMic || !kbdPadSurfaceEligible()) return false;
  // Reuse the manual Mic / Keys action; it owns countdown, capture and the
  // existing transport-failure/cancellation path. It never submits the draft.
  g2KbdPadHandleTap(kKbdRowMic);
  return gKbdPad.active && gKbdPad.page == kKbdPageMic;
}

// ─────────────────────────────────────────────────────────────────────
// Keyboard chunking A/B test — interactive, test-only session
// ─────────────────────────────────────────────────────────────────────
// Reached from Tests → Display Tests → Keyboard Tests. This deliberately
// does not reuse G2_Page_TextEntry or gKbdPad: it proves the mixed
// list+text+4-images surface and selective band pushes without sharing the
// production keyboard's state. The persistent session worker owns every
// state mutation and BLE wait; the ListEvent path above only queues FIFO taps.
namespace {

static constexpr const char* kKbdAbTextNames[2] = { "kbStat0", "kbStat1" };
static constexpr uint32_t    kKbdAbTextCid  = 97;
static constexpr size_t      kKbdAbShadeBytes =
    (size_t)kKbdImgW * (size_t)kKbdImgH;
static constexpr size_t      kKbdAbBmpCap =
    14 + 40 + 64 + ((size_t)kKbdImgW / 2 + 3) * kKbdImgH + 64;

static const char* const kKbdAbRows[] = {
  "<- Keyboard",
  "Up",
  "Left",
  "Right",
  "Down",
  "Next page",
  "Toggle 4 / 1",
  "Inject fallback",
  "Bench 23-step",
  "Show stats",
};
static constexpr size_t kKbdAbRowCount =
    sizeof(kKbdAbRows) / sizeof(kKbdAbRows[0]);

struct KbdAbBand {
  uint16_t y;
  uint16_t h;
  uint32_t cid;
  const char* name;
  uint32_t pushMagic;
};

// Full-width seams sit in the blank scanlines between key rows. This grouping
// is the page-toggle-weighted candidate from the host measurements; Q33 exists
// to measure that workload hypothesis on the actual link, not to call it
// universally optimal.
static constexpr KbdAbBand kKbdAbBands[4] = {
  {   0, 30, 2, "kbdAB0", G2_MAGIC_IMAGE_BASE +  1 },  // raw 2 frags
  {  30, 56, 3, "kbdAB1", G2_MAGIC_IMAGE_BASE +  3 },  // raw 3 frags
  {  86, 28, 4, "kbdAB2", G2_MAGIC_IMAGE_BASE +  6 },  // raw 2 frags
  { 114, 30, 5, "kbdAB3", G2_MAGIC_IMAGE_BASE +  8 },  // raw 2 frags
};
static constexpr uint32_t kKbdAbPushMagic1   = G2_MAGIC_IMAGE_BASE + 33;  // 243..248 raw

struct KbdAbStats {
  uint32_t actions;
  uint32_t tilePushes;
  uint32_t imageFrags;
  uint32_t rawBytes;
  uint32_t wireBytes;
  uint32_t totalMs;
  uint32_t minMs;
  uint32_t maxMs;
  uint32_t failures;
};

struct KbdAbCursor {
  uint8_t page;
  int8_t r;
  int8_t c;
};

struct KbdAbState {
  G2Temple* arm;
  uint32_t connectionGeneration;
  uint32_t lifecycleEpoch;
  uint32_t testGeneration;
  uint8_t nameBank;
  const char* listName;
  const char* textName;
  uint32_t presentationEpoch;
  bool fourTiles;
  bool fallback;
  bool surfaceLive;
  bool leaseHeld;
  uint8_t page;
  int8_t curR;
  int8_t curC;
  uint8_t lastMask;
  uint32_t actionSeq;
  uint8_t* storage;
  uint8_t* shades;
  uint8_t* painted;
  uint8_t* bmp;
  KbdAbStats stats[2];  // [0] one tile, [1] four bands
  KbdAbStats lastBench[2];
  bool benchValid[2];
  char fallbackReason[48];
};

struct KbdAbArgs {
  bool (*onDone)();
  uint32_t generation;
};

static bool kbdAbSameLink(const KbdAbState& s) {
  return s.arm && s.arm->connected && !s.arm->pluginDead &&
         s.arm->connectionGeneration == s.connectionGeneration;
}

static void kbdAbHeapLog(const char* where) {
  DEBUG_G2F("[KbdAB] heap %s: internal=%u largest=%u psram=%u",
            where ? where : "?",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static uint32_t kbdAbAvgMs(const KbdAbStats& st) {
  return st.actions ? st.totalMs / st.actions : 0;
}

static void kbdAbFormatStatus(const KbdAbState& s, char* out, size_t cap) {
  if (!out || cap == 0) return;
  const KbdAbStats& four = s.stats[1];
  const KbdAbStats& one  = s.stats[0];
  char fourBench[16];
  char oneBench[16];
  if (s.benchValid[1])
    snprintf(fourBench, sizeof(fourBench), "%u",
             (unsigned)kbdAbAvgMs(s.lastBench[1]));
  else
    snprintf(fourBench, sizeof(fourBench), "-");
  if (s.benchValid[0])
    snprintf(oneBench, sizeof(oneBench), "%u",
             (unsigned)kbdAbAvgMs(s.lastBench[0]));
  else
    snprintf(oneBench, sizeof(oneBench), "-");
  const uint32_t drops =
      __atomic_load_n(&gKbdAbTapDrops, __ATOMIC_ACQUIRE);
  const char* mode = s.fallback ? "1F" : (s.fourTiles ? "4B" : "1T");
  char tail[32];
  if (s.fallback && s.fallbackReason[0])
    snprintf(tail, sizeof(tail), "d%u fb:%.18s", (unsigned)drops,
             s.fallbackReason);
  else
    snprintf(tail, sizeof(tail), "tap fifo drops:%u", (unsigned)drops);
  // Four compact lines fit the 288x112 status pane without hiding the
  // benchmark/drop fields behind text wrapping. Full max/failure/reason data
  // stays in DEBUG_G2 logs.
  snprintf(out, cap,
           "KBD A/B %s p%ur%dc%d\n"
           "4 n%u t%u a%u b%s\n"
           "1 n%u t%u a%u b%s\n"
           "%s",
           mode, (unsigned)s.page, (int)s.curR, (int)s.curC,
           (unsigned)four.actions, (unsigned)four.tilePushes,
           (unsigned)kbdAbAvgMs(four), fourBench,
           (unsigned)one.actions, (unsigned)one.tilePushes,
           (unsigned)kbdAbAvgMs(one), oneBench, tail);
  out[cap - 1] = '\0';
}

static bool kbdAbUpdateStatus(KbdAbState& s) {
  if (!kbdAbSameLink(s) || !s.surfaceLive) return false;
  char text[224];
  kbdAbFormatStatus(s, text, sizeof(text));
  return sendUpdateTextNamed(*s.arm, s.textName, kKbdAbTextCid, text,
                             s.connectionGeneration);
}

// Send SHUTDOWN even after a CREATE timeout, where local containerReady is
// false but the firmware may have accepted a late/partial presentation.
// This is page-swap scoped (not ImageProbing), so it never calls
// imageProbeEnd.
static bool kbdAbForceShutdown(KbdAbState& s) {
  if (!kbdAbSameLink(s)) return false;
  const bool ok = tearDownActiveContainerAtGeneration(
      *s.arm, s.connectionGeneration, /*force*/ true);
  if (ok) s.surfaceLive = false;
  return ok;
}

static void kbdAbBuildTiles(bool fourTiles, G2ImageTile* out,
                            size_t* outCount) {
  if (!out || !outCount) return;
  if (!fourTiles) {
    out[0] = { 280, 128, (uint32_t)kKbdImgW, (uint32_t)kKbdImgH,
               kKbdImgCid, kKbdImgName };
    *outCount = 1;
    return;
  }
  for (size_t i = 0; i < 4; i++) {
    out[i] = { 280, (uint32_t)(128 + kKbdAbBands[i].y),
               (uint32_t)kKbdImgW, (uint32_t)kKbdAbBands[i].h,
               kKbdAbBands[i].cid, kKbdAbBands[i].name };
  }
  *outCount = 4;
}

static bool kbdAbCreate(KbdAbState& s, bool fourTiles) {
  if (!kbdAbSameLink(s) || !g2LensGetState().hijackActive ||
      gImgProbeAbort) return false;
  G2ImageTile tiles[4] = {};
  size_t tileCount = 0;
  kbdAbBuildTiles(fourTiles, tiles, &tileCount);

  char status[224];
  s.fourTiles = fourTiles;
  kbdAbFormatStatus(s, status, sizeof(status));
  G2TextChildSpec text = {};
  text.containerName = s.textName;
  text.content       = status;
  text.containerId   = kKbdAbTextCid;
  text.geom          = kKbdBufGeom;
  text.eventCapture  = false;

  constexpr size_t kPbCap = 1400;
  uint8_t* pb = (uint8_t*)ps_alloc(kPbCap, AllocPref::PreferPsram,
                                   "g2.pb.kbd-ab");
  if (!pb) return false;
  const uint32_t magic = allocCreateMagic();
  const size_t pbLen = g2BuildCreateMixedListTextImagesPb(
      magic, s.listName, kKbdAbRows, kKbdAbRowCount, kKbdListGeom,
      text, tiles, tileCount, BLOCKS_WIDGET_ID,
      G2_LTI_ORDER_LIST_TEXT_IMAGE, pb, kPbCap);
  if (!pbLen) {
    free(pb);
    DEBUG_G2F("[KbdAB] CREATE build failed (mode=%u)", fourTiles ? 4u : 1u);
    return false;
  }

  if (!probePrepImageCreateAck(magic, s.arm, s.connectionGeneration)) {
    free(pb);
    return false;
  }
  G2ImageCreateFence createFence = {};
  if (!captureImageCreateFence(*s.arm, &createFence)) {
    g2CreateTxnRelease();
    free(pb);
    return false;
  }
  // A fresh firmware MenuStartUp can invalidate this presentation while the
  // protobuf is being built. Do not put a now-stale CREATE on the wire after
  // its cooperative abort has already won.
  if (!kbdAbSameLink(s) || !g2LensGetState().hijackActive ||
      gImgProbeAbort) {
    g2CreateTxnRelease();
    free(pb);
    return false;
  }
  const uint32_t started = millis();
  const bool sent = g2SendCreateTxnPb(*s.arm, pb, pbLen);
  free(pb);
  if (!sent) g2CreateTxnRelease();
  const bool acked = sent && probeWaitImageCreateAck(
      magic, kImgCreateAckTimeoutMs, s.arm, createFence.generation,
      /*retainForPublication*/ true);
  DEBUG_G2F("[KbdAB] CREATE request=%u pb=%uB ack=%u ms=%u",
            fourTiles ? 4u : 1u, (unsigned)pbLen, acked ? 1u : 0u,
            (unsigned)(millis() - started));
  if (!acked) return false;
  if (!kbdAbSameLink(s) || !g2LensGetState().hijackActive ||
      gImgProbeAbort) {
    g2CompensateRejectedCreate(*s.arm, "kbd-ab surface revoked");
    g2CreateTxnRelease();
    return false;
  }

  // The list child owns event capture, so this is list-driven even though it
  // has text/image siblings.
  if (!g2NoteCreateSuccess(*s.arm, /*isList*/ true, BLOCKS_WIDGET_ID,
                           createFence.lifecycleEpoch)) {
    return false;
  }
  s.connectionGeneration = createFence.generation;
  s.lifecycleEpoch = createFence.lifecycleEpoch;
  s.presentationEpoch = g2PresentationEpochCurrent();
  __atomic_store_n(&gKbdAbRecoveryEpoch, s.presentationEpoch,
                   __ATOMIC_RELEASE);
  gLastHijackListRowCount  = kKbdAbRowCount;
  gLastHijackListPageShape = HijackListPageShape::ListWithTitle;
  g2TextViewDisarm();
  s.surfaceLive = true;
  return true;
}

static bool kbdAbBandChanged(const uint8_t* a, const uint8_t* b,
                             uint16_t y, uint16_t h) {
  return memcmp(a + (size_t)y * kKbdImgW,
                b + (size_t)y * kKbdImgW,
                (size_t)h * kKbdImgW) != 0;
}

static bool kbdAbPushRendered(KbdAbState& s, const char* action,
                              const KbdAbCursor& before,
  bool forceFull, bool injectAfterTile0,
                              bool countAction) {
  if (!kbdAbSameLink(s) || !s.surfaceLive) return false;
  const uint32_t started = millis();
  kbdRenderShades(s.shades, s.page, s.curR, s.curC);

  uint8_t mask = 0;
  if (s.fourTiles) {
    for (size_t i = 0; i < 4; i++) {
      if (forceFull || kbdAbBandChanged(s.shades, s.painted,
                                        kKbdAbBands[i].y,
                                        kKbdAbBands[i].h))
        mask |= (uint8_t)(1u << i);
    }
  } else if (forceFull || memcmp(s.shades, s.painted, kKbdAbShadeBytes) != 0) {
    mask = 1;
  }

  unsigned pushedTiles = 0;
  unsigned imageFrags = 0;
  size_t rawBytes = 0;
  size_t wireBytes = 0;
  bool ok = true;

  const size_t tileCount = s.fourTiles ? 4 : 1;
  for (size_t i = 0; i < tileCount && ok; i++) {
    if ((mask & (1u << i)) == 0) continue;
    const uint16_t y = s.fourTiles ? kKbdAbBands[i].y : 0;
    const uint16_t h = s.fourTiles ? kKbdAbBands[i].h : kKbdImgH;
    const uint32_t cid = s.fourTiles ? kKbdAbBands[i].cid : kKbdImgCid;
    const char* name = s.fourTiles ? kKbdAbBands[i].name : kKbdImgName;
    const uint32_t magic = s.fourTiles ? kKbdAbBands[i].pushMagic
                                       : kKbdAbPushMagic1;
    const size_t bmpLen = g2BuildBmp4bppFromShades(
        s.bmp, kKbdAbBmpCap, s.shades + (size_t)y * kKbdImgW,
        kKbdImgW, h);
    if (!bmpLen) { ok = false; break; }

    unsigned okFrags = 0, totalFrags = 0;
    size_t thisWireBytes = 0;
    char tag[20];
    snprintf(tag, sizeof(tag), "KbdAB/%s%u",
             s.fourTiles ? "b" : "full", (unsigned)i);
    ok = sendImageBmpFragmentsNoCreate(
        *s.arm, tag, magic, cid, name, s.bmp, bmpLen,
        &okFrags, &totalFrags,
        /*tolerateMissedAcks*/ 0, /*inFlightCapOverride*/ 0,
        G2_IMG_COMPRESS_RAW, &thisWireBytes, s.connectionGeneration,
        /*markExpectedEcho*/ false, s.lifecycleEpoch);
    pushedTiles++;
    imageFrags += totalFrags;
    rawBytes += bmpLen;
    wireBytes += thisWireBytes;

    // Safe failure injection: tile 0 is valid and fully acknowledged, then
    // we stop locally before tile 1. No malformed protobuf reaches firmware.
    if (ok && injectAfterTile0 && s.fourTiles && i == 0) {
      DEBUG_G2F("[KbdAB] injected failure after tile0 ack");
      ok = false;
      break;
    }
  }

  const uint32_t elapsed = millis() - started;
  s.lastMask = mask;
  if (ok) memcpy(s.painted, s.shades, kKbdAbShadeBytes);

  if (countAction) {
    KbdAbStats& st = s.stats[s.fourTiles ? 1 : 0];
    st.actions++;
    st.tilePushes += pushedTiles;
    st.imageFrags += imageFrags;
    st.rawBytes += (uint32_t)rawBytes;
    st.wireBytes += (uint32_t)wireBytes;
    st.totalMs += elapsed;
    if (st.actions == 1 || elapsed < st.minMs) st.minMs = elapsed;
    if (elapsed > st.maxMs) st.maxMs = elapsed;
    if (!ok) st.failures++;
  }

  DEBUG_G2F("[KbdAB] seq=%u mode=%u action=%s "
            "p%u:r%dc%d->p%u:r%dc%d mask=0x%02x tiles=%u "
            "raw=%u wire=%u imgfrags=%u push=%ums ok=%u",
            (unsigned)++s.actionSeq, s.fourTiles ? 4u : 1u,
            action ? action : "?",
            (unsigned)before.page, (int)before.r, (int)before.c,
            (unsigned)s.page, (int)s.curR, (int)s.curC,
            (unsigned)mask, pushedTiles, (unsigned)rawBytes,
            (unsigned)wireBytes, imageFrags, (unsigned)elapsed,
            ok ? 1u : 0u);
  return ok;
}

static bool kbdAbWaitForFsmState(const KbdAbState& s,
                                 HijackState wanted,
                                 const char* phase) {
  // Dispatch is asynchronous. Waiting only "while PageSwapping" can accept
  // the old Hijacked state before PageSwapBegin has drained, so require the
  // exact state on both sides of the mutation.
  if (!kbdAbSameLink(s) || !g2LensGetState().hijackActive ||
      gImgProbeAbort) return false;
  const uint32_t started = millis();
  while (hijackFsmState() != wanted) {
    if (!kbdAbSameLink(s) ||
        !g2LensGetState().hijackActive ||
        gImgProbeAbort ||
        (uint32_t)(millis() - started) >= 500u) {
      DEBUG_G2F("[KbdAB] FSM %s wait failed: wanted=%s got=%s",
                phase ? phase : "?", hijackStateName(wanted),
                hijackStateName(hijackFsmState()));
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

static void kbdAbRememberFallback(KbdAbState& s, const char* reason,
                                  bool injected) {
  s.fallback = true;
  snprintf(s.fallbackReason, sizeof(s.fallbackReason), "%s",
           (reason && reason[0]) ? reason
                                : (injected
                                       ? "injected after tile 0"
                                       : "4-band create/push failed"));
}

// Transactionally replace the current test surface. The lease excludes every
// ordinary page producer for the full session; PageSwapping itself covers only
// SHUTDOWN/CREATE. Initial pixels are sent after PageSwapEnd so real firmware
// exits are never misclassified as our teardown echoes.
static bool kbdAbTransition(KbdAbState& s, bool requestFour,
                            bool injectAfterTile0, const char* reason,
                            bool fallbackIntent = false) {
  if (!s.leaseHeld || !kbdAbSameLink(s) ||
      !g2LensGetState().hijackActive || gImgProbeAbort) return false;
  if (!pageSwapBeginWithInteractiveLease()) {
    DEBUG_G2F("[KbdAB] transition refused: another page swap owns admission");
    return false;
  }
  if (!kbdAbWaitForFsmState(s, HijackState::PageSwapping, "begin")) {
    pageSwapCancel();
    return false;
  }

  if (!tearDownActiveContainerAtGeneration(*s.arm, s.connectionGeneration,
                                            /*force*/ false)) {
    clearOurShutdownStamp();
    pageSwapCancel();
    DEBUG_G2F("[KbdAB] transition teardown failed");
    return false;
  }
  s.surfaceLive = false;
  if (!kbdAbSameLink(s) || !g2LensGetState().hijackActive ||
      gImgProbeAbort) {
    clearOurShutdownStamp();
    pageSwapFail("kbdAbTransition-aborted-after-teardown");
    return false;
  }
  if (fallbackIntent) {
    kbdAbRememberFallback(s, reason, injectAfterTile0);
  } else {
    s.fallback = false;
    s.fallbackReason[0] = '\0';
  }

  bool created = kbdAbCreate(s, requestFour);
  if (!created && requestFour && kbdAbSameLink(s) &&
      g2LensGetState().hijackActive && !gImgProbeAbort) {
    // CREATE refusal/timeout can still leave a late or partial presentation.
    // Quarantine it with an exact-generation SHUTDOWN before the disjoint
    // one-tile CREATE bank is armed.
    if (kbdAbForceShutdown(s)) {
      kbdAbRememberFallback(s, reason, injectAfterTile0);
      created = kbdAbCreate(s, /*fourTiles*/ false);
    }
  }

  if (!created) {
    (void)kbdAbForceShutdown(s);
    clearOurShutdownStamp();
    if (kbdAbSameLink(s) && g2LensGetState().hijackActive)
      pageSwapEnd();
    else
      pageSwapFail("kbdAbTransition");
    DEBUG_G2F("[KbdAB] transition failed completely");
    return false;
  }

  // The teardown echo window has already had its 500 ms settle. Do not let
  // the defensive 2 s wall stamp swallow a real exit during image painting.
  clearOurShutdownStamp();
  pageSwapEnd();
  if (!kbdAbWaitForFsmState(s, HijackState::Hijacked, "end")) {
    // CREATE may already have ACKed. If the hijack is still live, remove that
    // exact-generation surface rather than leaving an uncontrolled container.
    if (kbdAbSameLink(s) && s.surfaceLive)
      (void)kbdAbForceShutdown(s);
    clearOurShutdownStamp();
    pageSwapFail("kbdAbTransition-end-fence");
    return false;
  }

  memset(s.painted, 0xff, kKbdAbShadeBytes);  // shades are only 0..15
  const KbdAbCursor here = { s.page, s.curR, s.curC };
  const bool painted = kbdAbPushRendered(
      s, "initial", here, /*forceFull*/ true, injectAfterTile0,
      /*countAction*/ false);
  if (!painted && requestFour && kbdAbSameLink(s) &&
      g2LensGetState().hijackActive && !gImgProbeAbort) {
    return kbdAbTransition(s, /*requestFour*/ false,
                           /*injectAfterTile0*/ false,
                           (reason && reason[0]) ? reason
                                                : (injectAfterTile0
                                                       ? "injected after tile 0"
                                                       : "4-band initial push failed"),
                           /*fallbackIntent*/ true);
  }
  if (!painted) return false;

  __atomic_store_n(&gKbdAbActive, true, __ATOMIC_RELEASE);
  kbdAbHeapLog(s.fallback ? "fallback-live" :
               (s.fourTiles ? "four-live" : "one-live"));
  return true;
}

enum class KbdAbAction : uint8_t { Up, Left, Right, Down, NextPage };

static bool kbdAbApplyAction(KbdAbState& s, KbdAbAction action,
                             const char* label) {
  const KbdAbCursor before = { s.page, s.curR, s.curC };
  const char (*tbl)[kKbdCols + 1] = kbdPageTable(s.page);
  switch (action) {
    case KbdAbAction::Up:
    case KbdAbAction::Down: {
      const int dir = (action == KbdAbAction::Up) ? -1 : 1;
      s.curR = (int8_t)((s.curR + dir + kKbdRows) % kKbdRows);
      s.curC = (int8_t)kbdSpanStart(tbl, s.curR, s.curC);
      break;
    }
    case KbdAbAction::Left: {
      int c = (s.curC == 0) ? kKbdCols - 1 : s.curC - 1;
      s.curC = (int8_t)kbdSpanStart(tbl, s.curR, c);
      break;
    }
    case KbdAbAction::Right: {
      int c = kbdSpanEnd(tbl, s.curR, s.curC) + 1;
      if (c >= kKbdCols) c = 0;
      s.curC = (int8_t)c;
      break;
    }
    case KbdAbAction::NextPage:
      s.page = (uint8_t)((s.page + 1) % 3);
      tbl = kbdPageTable(s.page);
      s.curC = (int8_t)kbdSpanStart(tbl, s.curR, s.curC);
      break;
  }

  if (kbdAbPushRendered(s, label, before,
                        /*forceFull*/ false,
                        /*injectAfterTile0*/ false,
                        /*countAction*/ true))
    return true;

  // Live four-band failures use the same exact one-tile recovery as initial
  // startup. A one-tile failure ends the test and lets onDone restore the
  // Keyboard Tests picker.
  if (s.fourTiles && kbdAbSameLink(s) && !gImgProbeAbort) {
    return kbdAbTransition(s, /*requestFour*/ false,
                           /*injectAfterTile0*/ false,
                           "live band push failed",
                           /*fallbackIntent*/ true);
  }
  return false;
}

static bool kbdAbRunBench(KbdAbState& s) {
  const bool startedFour = s.fourTiles;
  const size_t statsIdx = startedFour ? 1u : 0u;
  // Canonicalize every run so the span remapping on the wide space key cannot
  // make two A/B samples start from different cursor/page states.
  if (s.page != 0 || s.curR != 1 || s.curC != 0) {
    const KbdAbCursor before = { s.page, s.curR, s.curC };
    s.page = 0;
    s.curR = 1;
    s.curC = 0;
    if (!kbdAbPushRendered(s, "bench-reset", before,
                           /*forceFull*/ false,
                           /*injectAfterTile0*/ false,
                           /*countAction*/ false)) {
      if (startedFour && kbdAbSameLink(s) && !gImgProbeAbort) {
        return kbdAbTransition(s, /*requestFour*/ false,
                               /*injectAfterTile0*/ false,
                               "bench reset band push failed",
                               /*fallbackIntent*/ true);
      }
      return false;
    }
  }
  // Give both modes the same deterministic pre-measurement transport and
  // compressor warm-up, independent of where the wearer entered Bench.
  {
    const KbdAbCursor here = { s.page, s.curR, s.curC };
    if (!kbdAbPushRendered(s, "bench-warm", here,
                           /*forceFull*/ true,
                           /*injectAfterTile0*/ false,
                           /*countAction*/ false)) {
      if (startedFour && kbdAbSameLink(s) && !gImgProbeAbort) {
        return kbdAbTransition(s, /*requestFour*/ false,
                               /*injectAfterTile0*/ false,
                               "bench warm-up band push failed",
                               /*fallbackIntent*/ true);
      }
      return false;
    }
  }
  s.stats[statsIdx] = {};
  const KbdAbAction seq[] = {
    KbdAbAction::Right, KbdAbAction::Right, KbdAbAction::Right,
    KbdAbAction::Right, KbdAbAction::Right,
    KbdAbAction::Left, KbdAbAction::Left, KbdAbAction::Left,
    KbdAbAction::Left, KbdAbAction::Left,
    KbdAbAction::Down, KbdAbAction::Down, KbdAbAction::Down,
    KbdAbAction::Down, KbdAbAction::Down,
    KbdAbAction::Up, KbdAbAction::Up, KbdAbAction::Up,
    KbdAbAction::Up, KbdAbAction::Up,
    KbdAbAction::NextPage, KbdAbAction::NextPage, KbdAbAction::NextPage,
  };
  DEBUG_G2F("[KbdAB] bench begin mode=%u steps=%u",
            startedFour ? 4u : 1u,
            (unsigned)(sizeof(seq) / sizeof(seq[0])));
  for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
    const char* label = (i < 5) ? "bench-R" :
                        (i < 10) ? "bench-L" :
                        (i < 15) ? "bench-D" :
                        (i < 20) ? "bench-U" : "bench-page";
    if (!kbdAbApplyAction(s, seq[i], label)) return false;
    if (s.fourTiles != startedFour) {
      DEBUG_G2F("[KbdAB] bench stopped: mode changed via fallback");
      return true;
    }
  }
  const KbdAbStats& st = s.stats[statsIdx];
  s.lastBench[statsIdx] = st;
  s.benchValid[statsIdx] = true;
  DEBUG_G2F("[KbdAB] bench done mode=%u actions=%u tiles=%u raw=%u "
            "wire=%u frags=%u avg=%ums min=%ums max=%ums failures=%u",
            startedFour ? 4u : 1u, (unsigned)st.actions,
            (unsigned)st.tilePushes, (unsigned)st.rawBytes,
            (unsigned)st.wireBytes, (unsigned)st.imageFrags,
            (unsigned)kbdAbAvgMs(st), (unsigned)st.minMs,
            (unsigned)st.maxMs, (unsigned)st.failures);
  (void)kbdAbUpdateStatus(s);
  return true;
}

static void kbdAbWorker(void* arg) {
  G2HijackCtxGuard ctxGuard;
  auto* a = (KbdAbArgs*)arg;
  KbdAbState s = {};
  s.testGeneration = a->generation;
  s.page = 0;
  s.curR = 1;
  s.curC = 0;
  bool restorePicker = false;

  do {
    if (g2TextEntryIsActive()) {
      DEBUG_G2F("[KbdAB] refused: production text entry is active");
      break;
    }
    s.arm = pickEvenAiArm("kbdAB");
    if (!s.arm) { DEBUG_G2F("[KbdAB] no eligible temple"); break; }
    s.connectionGeneration = s.arm->connectionGeneration;
    const uint32_t pickerEpoch = g2PresentationEpochCurrent();
    s.nameBank = (uint8_t)(s.testGeneration & 1u);
    s.listName = kKbdAbListNames[s.nameBank];
    s.textName = kKbdAbTextNames[s.nameBank];
    __atomic_store_n(&gKbdAbNameBank, s.nameBank, __ATOMIC_RELEASE);

    const uint32_t leaseStarted = millis();
    bool acquiredLease = false;
    while (!(acquiredLease = interactiveLensLeaseAcquire())) {
      if (!kbdAbSameLink(s) ||
          (uint32_t)(millis() - leaseStarted) >= 500u) {
        DEBUG_G2F("[KbdAB] could not acquire interactive lens lease");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    s.leaseHeld = acquiredLease;
    if (!s.leaseHeld) break;
    // The lease closes future page-swap/direct-text admission. Recheck what
    // was sampled before acquisition so a concurrently starting production
    // keyboard or newly accepted presentation is never overwritten.
    if (g2TextEntryIsActive() ||
        pickerEpoch != g2PresentationEpochCurrent() ||
        !g2LensGetState().hijackActive) {
      DEBUG_G2F("[KbdAB] launch lost original picker/text-entry race");
      break;
    }
    restorePicker = true;

    const size_t storageBytes = kKbdAbShadeBytes * 2 + kKbdAbBmpCap;
    // This is a pixel-truth A/B harness, not a production memory profile.
    // Require real SPIRAM so a 104 KB diagnostic allocation can never eat the
    // internal heap whose floor we are also trying to observe.
    s.storage = (uint8_t*)ps_alloc(storageBytes, AllocPref::RequirePsram,
                                   "g2.kbdab.storage");
    if (!s.storage) {
      DEBUG_G2F("[KbdAB] PSRAM alloc failed (%u B)",
                (unsigned)storageBytes);
      break;
    }
    s.shades  = s.storage;
    s.painted = s.shades + kKbdAbShadeBytes;
    s.bmp     = s.painted + kKbdAbShadeBytes;
    kbdAbHeapLog("start");

    kbdAbTapQueueReset();
    if (!kbdAbTransition(s, /*requestFour*/ true,
                         /*injectAfterTile0*/ false, nullptr)) {
      restorePicker = kbdAbSameLink(s) &&
                      g2LensGetState().hijackActive && !gImgProbeAbort;
      break;
    }
    __atomic_store_n(&gKbdAbStarting, false, __ATOMIC_RELEASE);

    bool keepRunning = true;
    while (keepRunning) {
      if (!kbdAbSameLink(s) || !g2LensGetState().hijackActive ||
          gImgProbeAbort ||
          s.testGeneration !=
              __atomic_load_n(&gKbdAbGeneration, __ATOMIC_ACQUIRE)) {
        DEBUG_G2F("[KbdAB] session aborted/stale");
        restorePicker = false;
        break;
      }

      uint8_t row = 0;
      if (!kbdAbTapDequeue(&row)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      if (row == 0) {
        DEBUG_G2F("[KbdAB] Back → exit");
        restorePicker = true;
        break;
      }

      switch (row) {
        case 1: keepRunning = kbdAbApplyAction(s, KbdAbAction::Up, "Up"); break;
        case 2: keepRunning = kbdAbApplyAction(s, KbdAbAction::Left, "Left"); break;
        case 3: keepRunning = kbdAbApplyAction(s, KbdAbAction::Right, "Right"); break;
        case 4: keepRunning = kbdAbApplyAction(s, KbdAbAction::Down, "Down"); break;
        case 5: keepRunning = kbdAbApplyAction(s, KbdAbAction::NextPage, "NextPage"); break;
        case 6:
          keepRunning = kbdAbTransition(s, !s.fourTiles,
                                        /*injectAfterTile0*/ false,
                                        "4-band mode switch failed");
          if (keepRunning) (void)kbdAbUpdateStatus(s);
          break;
        case 7:
          keepRunning = kbdAbTransition(s, /*requestFour*/ true,
                                        /*injectAfterTile0*/ true,
                                        "injected after tile 0");
          if (keepRunning) (void)kbdAbUpdateStatus(s);
          break;
        case 8: keepRunning = kbdAbRunBench(s); break;
        case 9: (void)kbdAbUpdateStatus(s); break;
        default: break;
      }
      if (!keepRunning) {
        restorePicker = kbdAbSameLink(s) &&
                        g2LensGetState().hijackActive && !gImgProbeAbort;
      }
    }
  } while (0);

  __atomic_store_n(&gKbdAbActive, false, __ATOMIC_RELEASE);
  __atomic_store_n(&gKbdAbStarting, false, __ATOMIC_RELEASE);
  kbdAbTapQueueReset();
  if (s.leaseHeld) {
    interactiveLensLeaseRelease();
    s.leaseHeld = false;
  }
  if (s.storage) free(s.storage);
  kbdAbHeapLog("exit");

  if (restorePicker) {
    const uint32_t settleStarted = millis();
    while (g2FsmPageSwapping() &&
           (uint32_t)(millis() - settleStarted) < 500u) {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  const bool callbackOk = restorePicker &&
      s.testGeneration ==
          __atomic_load_n(&gKbdAbGeneration, __ATOMIC_ACQUIRE) &&
      g2LensGetState().hijackActive && !gImgProbeAbort &&
      hijackFsmState() == HijackState::Hijacked;
  bool (*onDone)() = a->onDone;
  delete a;
  if (callbackOk && onDone) {
    if (onDone()) kbdAbRecoveryDisarm();
  } else if (s.testGeneration ==
                 __atomic_load_n(&gKbdAbGeneration, __ATOMIC_ACQUIRE) &&
             (!restorePicker ||
              !kbdAbRecoveryContextLive(__atomic_load_n(
                  &gKbdAbRecoveryEpoch, __ATOMIC_ACQUIRE)))) {
    // A real DISPLAY_OFF/SYSTEM_EXIT deliberately suppresses restoration.
    // Do not let a delayed ListEvent from the old Q33 surface resurrect the
    // Keyboard Tests picker after the wearer has exited the hijack.
    kbdAbRecoveryDisarm();
  }
}

static void kbdAbDrop(void* p) {
  auto* a = (KbdAbArgs*)p;
  if (a && a->generation ==
          __atomic_load_n(&gKbdAbGeneration, __ATOMIC_ACQUIRE))
    __atomic_store_n(&gKbdAbStarting, false, __ATOMIC_RELEASE);
  delete a;
}

}  // namespace

bool g2ShowKeyboardChunkTest(bool (*onDone)()) {
  bool expected = false;
  if (__atomic_load_n(&gKbdAbActive, __ATOMIC_ACQUIRE) ||
      !__atomic_compare_exchange_n(&gKbdAbStarting, &expected, true,
                                   false, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    DEBUG_G2F("[KbdAB] launch refused: already active/starting");
    return false;
  }
  if (g2TextEntryIsActive()) {
    __atomic_store_n(&gKbdAbStarting, false, __ATOMIC_RELEASE);
    DEBUG_G2F("[KbdAB] launch refused: text entry active");
    return false;
  }

  auto* a = new (std::nothrow) KbdAbArgs;
  if (!a) {
    __atomic_store_n(&gKbdAbStarting, false, __ATOMIC_RELEASE);
    return false;
  }
  uint32_t gen = __atomic_add_fetch(&gKbdAbGeneration, 1u, __ATOMIC_ACQ_REL);
  if (gen == 0)
    gen = __atomic_add_fetch(&gKbdAbGeneration, 1u, __ATOMIC_ACQ_REL);
  a->onDone = onDone;
  a->generation = gen;
  __atomic_store_n(&gKbdAbRecoveryCallback, onDone, __ATOMIC_RELEASE);
  if (!g2SessionSubmit(kbdAbWorker, a, kbdAbDrop, "kbd_chunk_ab",
                       __atomic_load_n(&gWidgetLifecycleEpoch,
                                       __ATOMIC_ACQUIRE))) {
    kbdAbDrop(a);
    return false;
  }
  return true;
}

bool g2KeyboardChunkTestRunning() {
  return __atomic_load_n(&gKbdAbActive, __ATOMIC_ACQUIRE) ||
         __atomic_load_n(&gKbdAbStarting, __ATOMIC_ACQUIRE);
}

#endif // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
