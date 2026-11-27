// ============================================================================
// OLED CLI Output - read-only browser of the local console history
// ============================================================================
// Three explicit states over OledConsoleBuffer's owned snapshots:
//   Live    follows new output. Up pauses on the record above the newest, A
//           reads the newest, Y jumps to the oldest retained, B leaves.
//   Paused  the selection is anchored by record ID while output keeps
//           arriving; the status row counts arrivals since the pause. Up/Down
//           browse (Down past the newest resumes Live), A reads, X resumes
//           Live, Y selects the oldest, B leaves.
//   Reader  an owned copy of one complete retained record, wrapped to the
//           screen. Up/Down scroll (by wheel detents) and continue into the
//           adjacent record past either end; Left/Right change records
//           directly; A/B return to the list on that record, X resumes Live,
//           Y opens the oldest.
// The page never holds the ring's mutex while drawing: each frame copies a
// status, a window of previews or one record, then draws from the copies. A
// busy ring draws a notice and asks for the next frame; new output invalidates
// the page through oledCliViewerNeedsFrame() in the dirty poll.
//
// What it shows is the OLED sink of the shared output queue: command output
// always, ambient and debug output when those flags are enabled. It is one
// mixed stream, not a history of commands typed on this display. Each record
// is one queue envelope (at most 255 bytes); a long command result arrives as
// several records, and upstream clamping is visible as the queue's own [CUT]
// mark. This page keeps no instrumentation of its own, so browsing never
// feeds the stream it displays.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include "HAL_Input.h"  // INPUT_CHECK / INPUT_BUTTON_*
#include "OLED_Utils.h"
#include "OLED_ConsoleBuffer.h"
#include "System_TextPager.h"

enum class CliView : uint8_t { LIVE, PAUSED, READER };

// Geometry: an 8-pixel status row, then 8-pixel record rows; one column each
// for the selection cursor and the scroll marks. 4 rows on the 128x64 OLED,
// 7 on the 128x88 Waveshare canvas, 12 on the P4X-EYE's 128x128 canvas.
// The live layout (10 columns and 9 rows in portrait); buffers are sized for
// the widest canvas and the most rows.
static const int kCliColsMax    = DISPLAY_WIDTH / 6;
static const int kCliRowsMax    = 24;
#define CLI_COLS        (SCREEN_WIDTH / 6)
#define CLI_LIST_COLS   (CLI_COLS - 2)
#define CLI_READER_COLS (CLI_COLS - 1)
static const int kCliRowH       = 8;
static const int kCliStatusH    = 9;
#define CLI_ROWS        (min(kCliRowsMax, (oledContentBottom() + 1 - OLED_CONTENT_START_Y - kCliStatusH) / kCliRowH))
#define CLI_ROWS_Y      (OLED_CONTENT_START_Y + kCliStatusH)
#define CLI_GUTTER_X    ((CLI_COLS - 1) * 6)
// Soft wraps add at most one '\n' per CLI_READER_COLS characters.
static const size_t kCliReaderWrapCap =
    oled_console::kRecordTextMax + oled_console::kRecordTextMax / 8 + 2;
static_assert(kCliColsMax <= (int)oled_console::kPreviewMax,
              "list previews must fit the console's preview copies");

static CliView  sView = CliView::LIVE;
static uint32_t sSelectedId = oled_console::kNoRecord;    // Paused selection
static uint32_t sWindowTopId = oled_console::kNoRecord;   // first visible Paused row
static uint32_t sPauseNewestId = oled_console::kNoRecord; // newest record when the pause began
static bool     sSelectionExpired = false;  // the paused selection was evicted
static bool     sReaderFromLive = false;    // Reader opened out of Live
static oled_console::RecordCopy sReader;    // owned Reader copy; survives eviction
static int      sReaderTop = 0;             // first visible wrapped line
static uint32_t sRenderedGeneration = 0;
static bool     sRetryPending = false;      // a busy ring left the last frame incomplete
// The footer draws "Back" as an 8-pixel icon, so a hint may run four glyphs
// past the column count and still fit.
static char     sFooterHint[32] = "B:Back";

static bool cliRetained(uint32_t id, const oled_console::Status& st) {
  return st.count > 0 && (id - st.oldestId) < st.count;  // IDs are contiguous
}

static void cliWipeReader() {
  memset(&sReader, 0, sizeof(sReader));
  sReaderTop = 0;
}

static void cliEnterLive() {
  sView = CliView::LIVE;
  sSelectedId = oled_console::kNoRecord;
  sWindowTopId = oled_console::kNoRecord;
  sSelectionExpired = false;
  cliWipeReader();
}

// A paused selection that has been evicted moves to the oldest survivor and
// says so until the next explicit navigation.
static void cliResolveSelection(const oled_console::Status& st) {
  if (st.count == 0) { cliEnterLive(); return; }
  if (!cliRetained(sSelectedId, st)) {
    sSelectedId = st.oldestId;
    sSelectionExpired = true;
  }
}

static void cliSelect(uint32_t id) {
  sSelectedId = id;
  sSelectionExpired = false;
}

// Pauses on `id`. `newPause` restarts the arrival count (a pause out of Live);
// returning from a Reader opened while paused keeps the original count.
static void cliEnterPaused(uint32_t id, const oled_console::Status& st, bool newPause) {
  if (st.count == 0) { cliEnterLive(); return; }
  if (newPause || sView == CliView::LIVE) sPauseNewestId = st.newestId;
  sView = CliView::PAUSED;
  cliSelect(id);
  cliResolveSelection(st);
  cliWipeReader();
}

// Copies record `id` into the owned Reader buffer. A busy ring asks for a
// retry frame; a record that is gone leaves the current view unchanged.
static bool cliReaderShow(uint32_t id) {
  oled_console::RecordCopy copy;
  bool busy = false;
  if (!gOledConsole.tryCopyRecord(id, copy, &busy)) {
    if (busy) sRetryPending = true;
    return false;
  }
  sReader = copy;
  sReaderTop = 0;
  sView = CliView::READER;
  return true;
}

// Opens the Reader from a list; where it returns to is fixed here, not by the
// records browsed inside it.
static bool cliOpenReader(uint32_t id, bool fromLive) {
  if (!cliReaderShow(id)) return false;
  sReaderFromLive = fromLive;
  return true;
}

// Back to the list on the record that was open: Live only when the Reader was
// opened from Live and still shows the newest record.
static void cliCloseReader(const oled_console::Status& st) {
  const uint32_t id = sReader.id;
  if (sReaderFromLive && st.count > 0 && id == st.newestId) {
    cliEnterLive();
  } else {
    sView = CliView::PAUSED;
    cliEnterPaused(id, st, sReaderFromLive);
  }
}

// Wraps the owned record for the screen and returns its line count.
static int cliWrapReader(char* out, size_t cap) {
  bool truncated = false;
  const size_t n = textWrapInto(out, cap, sReader.text, CLI_READER_COLS, 0, true, &truncated);
  if (n == 0) return 0;
  int lines = 1;
  for (size_t i = 0; i < n; ++i) if (out[i] == '\n') ++lines;
  return lines;
}

static void cliReaderLine(const char* wrapped, int index, char* out, size_t cap) {
  out[0] = '\0';
  const char* p = wrapped;
  for (int i = 0; i < index && p; ++i) {
    p = strchr(p, '\n');
    if (p) ++p;
  }
  if (!p) return;
  size_t n = 0;
  while (p[n] && p[n] != '\n' && n + 1 < cap) { out[n] = p[n]; ++n; }
  out[n] = '\0';
}

static void cliScrollReader(int delta, int lines) {
  const int maxTop = lines > CLI_ROWS ? lines - CLI_ROWS : 0;
  sReaderTop += delta;
  if (sReaderTop > maxTop) sReaderTop = maxTop;
  if (sReaderTop < 0) sReaderTop = 0;
}

// Moves the Reader to the adjacent record. `fromEnd` opens an older record at
// its last page, so scrolling up out of a record keeps reading backwards; an
// expired record has nothing older and continues at the oldest survivor.
static void cliReaderNeighbor(bool newer, bool fromEnd, const oled_console::Status& st) {
  if (st.count == 0) return;
  const bool retained = cliRetained(sReader.id, st);
  uint32_t id;
  if (newer) {
    if (sReader.id == st.newestId) return;
    id = retained ? sReader.id + 1 : st.oldestId;
  } else {
    if (!retained || sReader.id == st.oldestId) return;
    id = sReader.id - 1;
  }
  if (!cliReaderShow(id) || !fromEnd) return;
  char wrapped[kCliReaderWrapCap];
  const int lines = cliWrapReader(wrapped, sizeof(wrapped));
  cliScrollReader(lines, lines);  // clamps to the last page
}

// Status row: at most CLI_COLS glyphs so it never wraps into the rows.
static const size_t kCliStatusScratch = 48;  // holds any status before the column cut
static void cliDrawStatus(const char* text) {
  char row[kCliColsMax + 1];
  snprintf(row, sizeof(row), "%.*s", CLI_COLS, text);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print(row);
}

static void cliDrawMarks(bool olderAbove, bool newerBelow) {
  if (olderAbove) {
    oledDisplay->setCursor(CLI_GUTTER_X, CLI_ROWS_Y);
    oledDisplay->print("\x18");
  }
  if (newerBelow) {
    oledDisplay->setCursor(CLI_GUTTER_X, CLI_ROWS_Y + (CLI_ROWS - 1) * kCliRowH);
    oledDisplay->print("\x19");
  }
}

static void cliSetHint(const char* hint) {
  snprintf(sFooterHint, sizeof(sFooterHint), "%s", hint);
}

static void cliDrawList(const oled_console::Status& st) {
  int selected, top;
  if (sView == CliView::LIVE) {
    selected = st.count - 1;
    top = st.count > CLI_ROWS ? st.count - CLI_ROWS : 0;
  } else {
    cliResolveSelection(st);
    selected = (int)(sSelectedId - st.oldestId);
    top = cliRetained(sWindowTopId, st) ? (int)(sWindowTopId - st.oldestId) : 0;
    if (selected < top) top = selected;
    if (selected >= top + CLI_ROWS) top = selected - CLI_ROWS + 1;
    const int maxTop = st.count > CLI_ROWS ? st.count - CLI_ROWS : 0;
    if (top > maxTop) top = maxTop;
    if (top < 0) top = 0;
    sWindowTopId = st.oldestId + (uint32_t)top;
  }

  oled_console::Preview rows[kCliRowsMax];
  bool busy = false;
  const size_t shown = gOledConsole.tryCopyWindow(st.oldestId + (uint32_t)top, CLI_ROWS,
                                                  CLI_LIST_COLS, rows, &busy);
  if (shown == 0) {
    // Busy, or the window moved under the status snapshot: draw again next frame.
    sRetryPending = true;
    cliDrawStatus(busy ? "Console busy..." : "Updating...");
    cliSetHint("B:Back");
    return;
  }

  char status[kCliStatusScratch];
  if (sView == CliView::LIVE) {
    const uint32_t lost = gOledConsole.droppedCount();
    if (lost) snprintf(status, sizeof(status), "Live %u lost %lu", (unsigned)st.count, (unsigned long)lost);
    else      snprintf(status, sizeof(status), "Live %u", (unsigned)st.count);
  } else {
    const uint32_t arrivals = st.newestId - sPauseNewestId;
    char since[16] = "";
    if (arrivals) snprintf(since, sizeof(since), " +%lu", (unsigned long)arrivals);
    snprintf(status, sizeof(status), "%d/%u%s%s", selected + 1, (unsigned)st.count, since,
             sSelectionExpired ? " expired" : "");
  }
  cliDrawStatus(status);

  for (size_t i = 0; i < shown; ++i) {
    const int y = CLI_ROWS_Y + (int)i * kCliRowH;
    if (top + (int)i == selected) {
      oledDisplay->setCursor(0, y);
      oledDisplay->print(">");
    }
    char text[oled_console::kPreviewMax];
    snprintf(text, sizeof(text), "%s", rows[i].text);
    if (rows[i].cut) {  // the row shows less than the record holds
      size_t n = strlen(text);
      if ((int)n >= CLI_LIST_COLS) n = CLI_LIST_COLS - 1;
      text[n] = '~';
      text[n + 1] = '\0';
    }
    oledDisplay->setCursor(6, y);
    oledDisplay->print(text);
  }
  cliDrawMarks(top > 0, top + (int)shown < st.count);
  cliSetHint(sView == CliView::LIVE ? "A:Read Y:Old B:Back" : "A:Read X:Live B:Back");
}

static void cliDrawReader(const oled_console::Status& st) {
  char wrapped[kCliReaderWrapCap];
  const int lines = cliWrapReader(wrapped, sizeof(wrapped));
  cliScrollReader(0, lines);

  // The owned copy stays readable after eviction; the status row says so
  // instead of substituting another record.
  char status[kCliStatusScratch];
  const unsigned long ts = sReader.timestampMs;
  const char* cut = (sReader.flags & oled_console::kFlagTruncated) ? " ~" : "";
  if (cliRetained(sReader.id, st)) {
    snprintf(status, sizeof(status), "%lu/%u T+%lu.%03lus%s",
             (unsigned long)(sReader.id - st.oldestId + 1), (unsigned)st.count,
             ts / 1000UL, ts % 1000UL, cut);
  } else {
    snprintf(status, sizeof(status), "expired T+%lu.%03lus%s", ts / 1000UL, ts % 1000UL, cut);
  }
  cliDrawStatus(status);

  if (lines == 0) {
    oledDisplay->setCursor(0, CLI_ROWS_Y);
    oledDisplay->print("(empty)");
  }
  char line[kCliColsMax + 1];
  for (int row = 0; row < CLI_ROWS && sReaderTop + row < lines; ++row) {
    cliReaderLine(wrapped, sReaderTop + row, line, sizeof(line));
    oledDisplay->setCursor(0, CLI_ROWS_Y + row * kCliRowH);
    oledDisplay->print(line);
  }
  cliDrawMarks(sReaderTop > 0, sReaderTop + CLI_ROWS < lines);
  cliSetHint("\x18\x19:Scroll X:Live B:Back");
}

static void displayCliViewer() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  oled_console::Status st;
  if (!gOledConsole.tryStatus(st)) {
    sRetryPending = true;
    cliDrawStatus("Console busy...");
    cliSetHint("B:Back");
    return;
  }
  sRenderedGeneration = st.generation;
  sRetryPending = false;

  if (sView == CliView::READER) {
    cliDrawReader(st);
    return;
  }
  if (st.count == 0) {
    cliEnterLive();
    cliDrawStatus("Live");
    oledDisplay->setCursor(0, CLI_ROWS_Y);
    oledDisplay->print("No CLI output yet");
    cliSetHint("B:Back");
    return;
  }
  cliDrawList(st);
}

static bool handleCliViewerInput(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  oled_console::Status st;
  if (!gOledConsole.tryStatus(st)) {
    sRetryPending = true;
    return false;
  }
  const bool a = INPUT_CHECK(newlyPressed, INPUT_BUTTON_A);
  const bool b = INPUT_CHECK(newlyPressed, INPUT_BUTTON_B);
  const bool x = INPUT_CHECK(newlyPressed, INPUT_BUTTON_X);
  const bool y = INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y);

  if (sView == CliView::READER) {
    if (a || b) { cliCloseReader(st); return true; }
    if (x) { cliEnterLive(); return true; }
    if (y) {
      if (st.count > 0) cliReaderShow(st.oldestId);
      return true;
    }
    if (gNavEvents.left)  { cliReaderNeighbor(false, false, st); return true; }
    if (gNavEvents.right) { cliReaderNeighbor(true, false, st); return true; }
    if (gNavEvents.up || gNavEvents.down) {
      // Scroll by the wheel's detents (one line per joystick step). A wheel
      // has no Left/Right, so scrolling past either end continues into the
      // adjacent record; a fast spin stops at the end first.
      char wrapped[kCliReaderWrapCap];
      const int lines = cliWrapReader(wrapped, sizeof(wrapped));
      const int maxTop = lines > CLI_ROWS ? lines - CLI_ROWS : 0;
      int step = gNavEvents.wheelDelta < 0 ? -gNavEvents.wheelDelta : gNavEvents.wheelDelta;
      if (step < 1) step = 1;
      if (gNavEvents.down) {
        if (sReaderTop < maxTop) cliScrollReader(step, lines);
        else cliReaderNeighbor(true, false, st);
      } else {
        if (sReaderTop > 0) cliScrollReader(-step, lines);
        else cliReaderNeighbor(false, true, st);
      }
      return true;
    }
    return false;
  }

  if (st.count == 0) {
    cliEnterLive();
    return a || x || y;  // nothing to browse; B still leaves the page
  }

  if (sView == CliView::LIVE) {
    if (a) { cliOpenReader(st.newestId, true); return true; }
    if (x) return true;  // already following
    if (y) { cliEnterPaused(st.oldestId, st, true); return true; }
    if (gNavEvents.up) {
      cliEnterPaused(st.count > 1 ? st.newestId - 1 : st.newestId, st, true);
      return true;
    }
    if (gNavEvents.down) return true;
    return false;
  }

  // Paused
  cliResolveSelection(st);
  if (a) { cliOpenReader(sSelectedId, false); return true; }
  if (x) { cliEnterLive(); return true; }
  if (y) { cliSelect(st.oldestId); return true; }
  if (gNavEvents.up) {
    if (sSelectedId != st.oldestId) cliSelect(sSelectedId - 1);
    else sSelectionExpired = false;
    return true;
  }
  if (gNavEvents.down) {
    if (sSelectedId != st.newestId) cliSelect(sSelectedId + 1);
    else cliEnterLive();
    return true;
  }
  return false;
}

// Dirty-poll hook: the page needs a frame when the console changed since the
// last one or a busy ring left that frame incomplete.
bool oledCliViewerNeedsFrame() {
  if (currentOledMode != OLED_CLI_VIEWER) return false;
  return sRetryPending || gOledConsole.generation() != sRenderedGeneration;
}

// The shared footer draws this after the page renders each frame.
const char* oledCliViewerFooterHint() { return sFooterHint; }

// Identity boundary: the owned Reader copy and every anchor belong to the
// identity that is leaving.
void oledCliViewerResetSessionState() {
  cliEnterLive();
  sPauseNewestId = oled_console::kNoRecord;
  sRenderedGeneration = 0;
  sRetryPending = false;
  cliSetHint("B:Back");
}

static bool isCliViewerAvailable(String* outReason) {
  if (!gOledConsole.isReady()) {
    if (outReason) *outReason = "Console buffer not initialized";
    return false;
  }
  return true;
}

// Every entry opens Live on the newest output, forward or back.
static void cliViewerOnEnter(bool /*isForward*/) {
  oledCliViewerResetSessionState();
}

static const OledModeEntry kCliViewerEntry = {
  OLED_CLI_VIEWER,
  "CLI Output",
  "notify_system",
  displayCliViewer,
  isCliViewerAvailable,
  handleCliViewerInput,
  true,
  92,
  nullptr,  // state-specific hints via oledCliViewerFooterHint()
  cliViewerOnEnter
};

static const OledModeEntry kCliViewerModes[] = { kCliViewerEntry };

REGISTER_OLED_MODE_MODULE(kCliViewerModes, sizeof(kCliViewerModes) / sizeof(kCliViewerModes[0]), "CLIViewer");

#endif // ENABLE_OLED_DISPLAY
