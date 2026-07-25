// ============================================================================
// OLED LLM Chat Mode
// ============================================================================
// State-machine driven chat surface backed by System_LLMChat (the shared
// conversation module). Four sub-states:
//
//   NO_MODEL    — model isn't loaded. Footer: "A: pick model".
//   LOADING     — model load in progress (synchronous in engine; OLED freezes
//                 on the file picker's last frame while weights are read).
//   READY       — chat view with turn history. "A: ask  Y: retry".
//   GENERATING  — streaming the active turn, "B: stop".
//
// Model selection is now delegated to the shared file picker (OLED_Display.h's
// FilePickerRequest): NO_MODEL → A pushes a picker, the file browser opens
// scoped to /system/llm and filtered to *.bin, the callback loads the picked
// model. The prior inline picker (own ArduinoJson parse of llmListModels())
// is gone.
//
// Other carry-overs from the previous version:
//   - No local conversation buffer; turns come from chatGetTurnCount/InfoCopy.
//   - No private xTaskCreate'd worker; generation rides the engine's existing
//     async task and we pull-on-read each render.
//   - Settings actually apply (gSettings.llm* is consumed by chatBeginTurn).
//   - Retry is a single button press (Y), no retyping the prompt.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ONDEVICE_LLM

#include <Adafruit_SSD1306.h>

#include "OLED_Utils.h"
#include "HAL_Input.h"          // INPUT_CHECK, INPUT_BUTTON_*, gNavEvents
#include "System_LLM.h"
#include "System_LLMChat.h"
#include "System_Debug.h"
#include "System_MemUtil.h"

// ============================================================================
// Layout constants
// ============================================================================

static const int LLM_CHARS         = 21;   // Characters per line at text size 1
static const int LLM_LINE_H        = 10;   // Pixels per text line
static const int LLM_VISIBLE_LINES = 4;    // Chat lines visible above the footer
static const int LLM_FOOTER_H      = 10;   // Footer bar height

// Render scratch — flat-out copy of the conversation as wrapped lines. Sized
// to cover full visible window plus a couple of lines of headroom for the
// streaming case. Lives in DRAM but is small (~600 B).
static const int LLM_RENDER_LINES = 12;

// ============================================================================
// UI state
// ============================================================================

enum class LLMUIState : uint8_t {
  NO_MODEL = 0,
  LOADING,
  READY,
  GENERATING,
  // Guided-input menu pickers (LLM_GUIDED_MENU_SPEC §8) — nested sub-states
  // entered with X from READY when the loaded model ships a menu. Back (B) pops
  // one level: ENTITY -> TEMPLATE -> GROUP -> READY.
  PICK_GROUP,
  PICK_TEMPLATE,
  PICK_ENTITY,
};

static LLMUIState sUIState = LLMUIState::NO_MODEL;
static bool       sKeyboardActive = false;
static int        sScrollOffset = 0;        // 0 = newest at bottom; positive = scrolled back

// Render scratch — wrapped lines built from chatReadTurn() during display.
// Each "line" is a (turn-index, byte-offset-into-turn, length) triple plus a
// pre-wrapped 21-char snippet. Used to make scroll/wrap math local.
struct WrappedLine {
  char text[LLM_CHARS + 1];
  bool isUserPrompt;   // affects rendering (prepended ">" for user turns)
};
EXT_RAM_BSS_ATTR static WrappedLine sRenderLines[LLM_RENDER_LINES];
static int sRenderLineCount = 0;

// ============================================================================
// Guided-input menu picker state (LLM_GUIDED_MENU_SPEC §8)
// ============================================================================
// Three nested sub-states, each an OLEDScrollState (the per-mode picker pattern;
// the generic modal picker was deliberately removed — not recreated here). The
// shared System_LLM_Menu API composes + fires the question by integer index, then
// the normal GENERATING view streams the answer.

// Entities can number in the thousands, so the entity picker shows a windowed
// page of up to this many rows plus '< Prev' / 'Next >' edge rows (total <= 32,
// the OLEDScrollState item cap). Groups (<=8) and templates fit directly and just
// scroll within one list.
static const int LLM_ENT_PAGE = 29;

// OLEDScrollState stores string POINTERS (no copy), so each row's text must live
// in stable backing storage while its list is on screen. Rows are truncated to
// LLM_CHARS (display only — selection is by index). Kept in PSRAM: DRAM is tight
// and menu data is not secret.
EXT_RAM_BSS_ATTR static OLEDScrollState sGroupScroll;
EXT_RAM_BSS_ATTR static OLEDScrollState sTplScroll;
EXT_RAM_BSS_ATTR static OLEDScrollState sEntScroll;
static bool sPickScrollInit = false;

EXT_RAM_BSS_ATTR static char sGroupRows[8][LLM_CHARS + 1];
EXT_RAM_BSS_ATTR static char sTplRows[OLED_SCROLL_MAX_ITEMS][LLM_CHARS + 1];
EXT_RAM_BSS_ATTR static char sEntRows[LLM_ENT_PAGE][LLM_CHARS + 1];

// Generation stamped when the picker opens; every frame re-checks it so a model
// unload/swap (which bumps menuGeneration) pops us straight back out.
static uint16_t sPickGen   = 0;
static uint8_t  sPickGroup = 0;   // chosen group (valid in TEMPLATE/ENTITY)
static uint16_t sPickTpl   = 0;   // chosen template (valid in ENTITY)
static uint16_t sEntWindow = 0;   // index of the first entity on the current page
static uint16_t sEntTotal  = 0;   // entity count of the chosen group

// Transient status shown in the footer after a submit that couldn't start (busy).
// Cleared on the next navigation / selection. A string literal, so no backing buf.
static const char* sPickStatus = nullptr;

// Entity-list userData sentinels for the '< Prev' / 'Next >' edge rows. Entity
// rows carry (void*)(uintptr_t)(entityIndex + ENT_ROW_BASE) so they never collide
// with these or with a nullptr default.
static void* const ENT_ROW_PREV = (void*)1;
static void* const ENT_ROW_NEXT = (void*)2;
static const uintptr_t ENT_ROW_BASE = 3;

// Retry branching (spec §5): a guided ask must be re-run PLAIN via
// llmMenuRepeatLast() — chatRetryLast() would ban the memorized-correct answer.
// chatGetSessionId() reads 0 the instant a turn finishes, so "was the last turn
// guided?" can't be recovered from the chat module alone; track it locally. Set
// true on a guided submit, false on a free-text (keyboard) submit. The session id
// guards against another surface's guided ask having replaced the stored question.
static bool sLastTurnGuided   = false;
static int  sGuidedAskSession = 0;

// ============================================================================
// State transitions
// ============================================================================

// Refresh state from the engine + chat module. Called at the top of
// displayLLM() each frame so transitions land within one render tick.
static void syncStateFromEngine() {
  if (sKeyboardActive) return;  // keyboard owns the UI

  LLMStatus st = llmGetStatus();

  // Guided-menu pickers own the UI while active (spec §8). They leave only on an
  // explicit button — a submit sets GENERATING, B pops a level — or when the model
  // or its menu vanishes underneath us: an unload/swap bumps menuGeneration, and an
  // unload drops the engine out of READY. Checked before the generic transitions so
  // a background generation (another surface) or the reflect-engine branch below
  // can't yank the user out of the picker mid-selection.
  if (sUIState == LLMUIState::PICK_GROUP ||
      sUIState == LLMUIState::PICK_TEMPLATE ||
      sUIState == LLMUIState::PICK_ENTITY) {
    if (st.state != LLMState::READY ||
        llmMenuGroupCount() == 0 ||
        llmMenuGeneration() != sPickGen) {
      // Model unloaded / swapped / menu gone — drop back to the chat view (which
      // itself renders the NO_MODEL screen when the model is truly gone).
      sUIState = (st.state == LLMState::READY) ? LLMUIState::READY : LLMUIState::NO_MODEL;
      sPickStatus = nullptr;
    }
    return;
  }

  // Generation overrides everything else
  if (chatIsGenerating()) {
    if (sUIState != LLMUIState::GENERATING) {
      sScrollOffset = 0;
      sUIState = LLMUIState::GENERATING;
    }
    return;
  }

  // From GENERATING → settle to READY when chat says it's done
  if (sUIState == LLMUIState::GENERATING) {
    sUIState = LLMUIState::READY;
    return;
  }

  // From LOADING → READY/NO_MODEL based on engine state
  if (sUIState == LLMUIState::LOADING) {
    if (st.state == LLMState::READY) {
      sUIState = LLMUIState::READY;
    } else if (st.state == LLMState::ERROR || st.state == LLMState::UNLOADED) {
      sUIState = LLMUIState::NO_MODEL;
    }
    return;
  }

  // Otherwise reflect whatever the engine reports.
  if (st.state == LLMState::READY) {
    sUIState = LLMUIState::READY;
  } else if (st.state == LLMState::LOADING) {
    sUIState = LLMUIState::LOADING;
  } else {
    sUIState = LLMUIState::NO_MODEL;
  }
}

// ============================================================================
// Model picker — shared file-browser-based picker, callback-driven
// ============================================================================

// Filter: only show files ending in .bin (case-insensitive on the extension —
// the LLM engine accepts any name but expects the LLM1 binary format, which
// the converter writes as .bin). Folders are passed through by the picker
// layer regardless of this filter so the user can navigate.
static bool isLLMModelFile(const FileEntry& entry) {
  const char* dot = strrchr(entry.name, '.');
  if (!dot) return false;
  return (strcasecmp(dot, ".bin") == 0);
}

// Callback for when the user picks a model file (or cancels). Runs after the
// file browser has popped back to OLED_LLM. llmLoadModel is synchronous and
// blocks for several seconds reading weights into PSRAM — during that window
// the OLED freezes on whatever frame was last drawn (the file browser at
// selection). State transitions to LOADING for completeness even though no
// LOADING frame is rendered in practice; if a future async-load API arrives,
// the state machine is ready for it.
static void onLLMModelPicked(const char* fullPath, bool cancelled) {
  if (cancelled || !fullPath) {
    sUIState = LLMUIState::NO_MODEL;
    return;
  }
  sUIState = LLMUIState::LOADING;
  bool ok = llmLoadModel(fullPath, 0);  // 0 = use compile-time / settings max ctx
  sUIState = ok ? LLMUIState::READY : LLMUIState::NO_MODEL;
}

// Push the model-pick file picker request and transition to OLED_FILE_BROWSER.
// Called when the user presses A in the NO_MODEL state.
static void openModelPicker() {
  FilePickerRequest req = {};
  strlcpy(req.title, "Pick model", sizeof(req.title));
  // Start in /system/llm — the conventional model directory. The user can
  // navigate up to / and into /sd/llm if their model lives on SD.
  strlcpy(req.startPath, "/system/llm", sizeof(req.startPath));
  req.filter = isLLMModelFile;
  req.onPicked = onLLMModelPicked;
  req.requesterMode = OLED_LLM;
  if (oledFilePickerPush(req)) {
    requestOLEDMode(OLED_FILE_BROWSER, "llm.pickModel", true);
  }
}

// ============================================================================
// Guided-input menu pickers — build/refill lists from the shared C API
// ============================================================================

static void ensurePickScrollInit() {
  if (sPickScrollInit) return;
  int vis = OLED_CONTENT_HEIGHT / 8;   // single-line (8px) rows in the content area
  if (vis < 1) vis = 1;
  oledScrollInit(&sGroupScroll, nullptr, vis);
  oledScrollInit(&sTplScroll,   nullptr, vis);
  oledScrollInit(&sEntScroll,   nullptr, vis);
  sPickScrollInit = true;
}

// Copy a template's display form into `out` (<= LLM_CHARS), collapsing the "{}"
// slot marker to a single '_' (spec §8: "slot renders as _"). llmMenuTemplate()
// is the only accessor and renders the 0x1F slot byte as "{}", so that two-char
// sequence is the sole slot source here.
static void tplDisplayRow(uint8_t g, uint16_t t, char* out, size_t cap, bool* hasSlot) {
  if (!out || cap == 0) return;
  char tmp[128];
  int n = llmMenuTemplate(g, t, tmp, sizeof(tmp), hasSlot);
  if (n < 0) { out[0] = '\0'; return; }
  size_t oi = 0;
  for (size_t i = 0; tmp[i] && oi + 1 < cap; i++) {
    if (tmp[i] == '{' && tmp[i + 1] == '}') {
      out[oi++] = '_';
      i++;                 // consume the '}' too
    } else {
      out[oi++] = tmp[i];
    }
  }
  out[oi] = '\0';
}

// Rebuilt every frame (oledScrollClearKeepSelection preserves the cursor). All
// row text is copied into stable backing storage because OLEDScrollState stores
// pointers, not copies.
static void populateGroupPicker() {
  ensurePickScrollInit();
  oledScrollClearKeepSelection(&sGroupScroll);
  uint8_t n = llmMenuGroupCount();
  if (n > 8) n = 8;                    // format cap; sGroupRows is sized to match
  for (uint8_t i = 0; i < n; i++) {
    LLMMenuGroupInfo gi;
    if (llmMenuGroupInfo(i, &gi)) {
      strlcpy(sGroupRows[i], gi.name, sizeof(sGroupRows[i]));   // 21-char truncation
    } else {
      sGroupRows[i][0] = '\0';
    }
    oledScrollAddItem(&sGroupScroll, sGroupRows[i]);
  }
  oledScrollClampSelection(&sGroupScroll);
}

static void populateTemplatePicker() {
  ensurePickScrollInit();
  oledScrollClearKeepSelection(&sTplScroll);
  LLMMenuGroupInfo gi;
  uint16_t n = llmMenuGroupInfo(sPickGroup, &gi) ? gi.tplCount : 0;
  // Curation targets 5-15 templates/group; the format allows up to 64. One
  // OLEDScrollState scrolls up to OLED_SCROLL_MAX_ITEMS, which comfortably covers
  // the curated range — cap here so a rogue overlong list can't overflow.
  if (n > OLED_SCROLL_MAX_ITEMS) n = OLED_SCROLL_MAX_ITEMS;
  for (uint16_t t = 0; t < n; t++) {
    bool hs = false;
    tplDisplayRow(sPickGroup, t, sTplRows[t], sizeof(sTplRows[t]), &hs);
    oledScrollAddItem(&sTplScroll, sTplRows[t]);
  }
  oledScrollClampSelection(&sTplScroll);
}

// Windowed entity page: an optional '< Prev' row, up to LLM_ENT_PAGE entity rows,
// then an optional 'Next >' row — refilled each frame from sEntWindow.
static void populateEntityPicker() {
  ensurePickScrollInit();
  oledScrollClearKeepSelection(&sEntScroll);

  if (sEntWindow >= sEntTotal) sEntWindow = 0;   // defensive re-clamp
  bool     hasPrev   = (sEntWindow > 0);
  uint16_t remaining = (sEntTotal > sEntWindow) ? (uint16_t)(sEntTotal - sEntWindow) : 0;
  uint16_t pageCount = remaining < LLM_ENT_PAGE ? remaining : (uint16_t)LLM_ENT_PAGE;
  bool     hasNext   = ((uint32_t)sEntWindow + pageCount) < sEntTotal;

  if (hasPrev) oledScrollAddItem(&sEntScroll, "< Prev", nullptr, true, ENT_ROW_PREV);
  for (uint16_t i = 0; i < pageCount; i++) {
    uint16_t entIdx = (uint16_t)(sEntWindow + i);
    if (llmMenuEntity(sPickGroup, entIdx, sEntRows[i], sizeof(sEntRows[i])) < 0) {
      sEntRows[i][0] = '\0';
    }
    oledScrollAddItem(&sEntScroll, sEntRows[i], nullptr, true,
                      (void*)(uintptr_t)(entIdx + ENT_ROW_BASE));
  }
  if (hasNext) oledScrollAddItem(&sEntScroll, "Next >", nullptr, true, ENT_ROW_NEXT);
  oledScrollClampSelection(&sEntScroll);
}

// Level entry helpers — reset the cursor to the top of the level being entered.
static void enterPickGroup() {
  ensurePickScrollInit();
  sPickGen    = llmMenuGeneration();
  sPickStatus = nullptr;
  sGroupScroll.selectedIndex = 0;
  sGroupScroll.scrollOffset  = 0;
  sUIState = LLMUIState::PICK_GROUP;
}

static void enterPickTemplate(uint8_t g) {
  sPickGroup  = g;
  sPickStatus = nullptr;
  sTplScroll.selectedIndex = 0;
  sTplScroll.scrollOffset  = 0;
  sUIState = LLMUIState::PICK_TEMPLATE;
}

static void enterPickEntity(uint16_t t) {
  sPickTpl    = t;
  sPickStatus = nullptr;
  LLMMenuGroupInfo gi;
  sEntTotal   = llmMenuGroupInfo(sPickGroup, &gi) ? gi.entCount : 0;
  sEntWindow  = 0;
  sEntScroll.selectedIndex = 0;
  sEntScroll.scrollOffset  = 0;
  sUIState = LLMUIState::PICK_ENTITY;
}

// Compose + submit the chosen (group, template, entity). e = -1 for a slotless
// (canned) template.
static void submitGuided(int entityIndex) {
  int rc = llmMenuAsk(sPickGen, sPickGroup, sPickTpl, entityIndex, nullptr);
  if (rc > 0) {
    sLastTurnGuided   = true;
    sGuidedAskSession = rc;
    sPickStatus       = nullptr;
    sScrollOffset     = 0;
    sUIState          = LLMUIState::GENERATING;   // stream the answer (existing view)
  } else if (rc == 0) {
    // Busy — a generation is already in flight. Stay in the picker; show status.
    sPickStatus = "Busy: answer in progress";
  } else {
    // -1 stale gen (model swapped) / -2 bad index / -3 no menu — bail to READY;
    // syncStateFromEngine reflects NO_MODEL from there if the model is truly gone.
    sPickStatus = nullptr;
    sUIState    = LLMUIState::READY;
  }
}

// ============================================================================
// Conversation rendering — build wrapped lines from the chat module
// ============================================================================

// Walk the conversation, wrap each turn into LLM_CHARS-wide lines, append into
// sRenderLines (newest at the end). Stops when sRenderLines is full — we keep
// the newest lines because scroll defaults to bottom.
static void rebuildRenderLines() {
  sRenderLineCount = 0;
  int turnCount = chatGetTurnCount();
  if (turnCount == 0) return;

  // Two-pass strategy: figure out total lines, then skip the oldest ones if
  // they'd overflow our scratch (favor newer content).
  // Pass 1: count lines for each turn into a small array.
  struct TurnLineSpan { int lineStart; int lineCount; };
  TurnLineSpan spans[LLM_CHAT_MAX_TURNS];
  int totalLines = 0;

  char turnBuf[LLM_CHAT_TURN_MAX_BYTES + 1];
  for (int i = 0; i < turnCount; i++) {
    spans[i].lineStart = totalLines;
    int copied = chatReadTurn(i, 0, turnBuf, sizeof(turnBuf));
    if (copied <= 0) {
      spans[i].lineCount = 0;
      continue;
    }
    // Count wrapped lines = ceil(copied / LLM_CHARS), minimum 1 if any chars.
    int lc = (copied + LLM_CHARS - 1) / LLM_CHARS;
    if (lc < 1) lc = 1;
    spans[i].lineCount = lc;
    totalLines += lc;
  }

  // Pass 2: emit the newest LLM_RENDER_LINES into sRenderLines.
  int skipLines = totalLines > LLM_RENDER_LINES ? totalLines - LLM_RENDER_LINES : 0;
  int emitted = 0;
  for (int i = 0; i < turnCount && emitted < LLM_RENDER_LINES; i++) {
    if (spans[i].lineCount == 0) continue;

    // Where does this turn's content overlap with the visible window?
    int turnLineStart = spans[i].lineStart;
    int firstWantedLineInTurn = skipLines > turnLineStart ? skipLines - turnLineStart : 0;
    if (firstWantedLineInTurn >= spans[i].lineCount) continue;

    ChatTurnInfo info;
    if (!chatGetTurnInfo(i, &info)) continue;

    int copied = chatReadTurn(i, 0, turnBuf, sizeof(turnBuf));
    if (copied <= 0) continue;

    for (int line = firstWantedLineInTurn;
         line < spans[i].lineCount && emitted < LLM_RENDER_LINES;
         line++) {
      int offset = line * LLM_CHARS;
      int len = copied - offset;
      if (len <= 0) break;
      if (len > LLM_CHARS) len = LLM_CHARS;
      WrappedLine& wl = sRenderLines[emitted];
      memcpy(wl.text, turnBuf + offset, len);
      wl.text[len] = '\0';
      wl.isUserPrompt = (info.role == ChatTurnRole::USER);
      emitted++;
    }
  }
  sRenderLineCount = emitted;
}

// ============================================================================
// Display
// ============================================================================

static void drawFooter(const char* line) {
  if (!line || !oledDisplay) return;
  int y = DISPLAY_HEIGHT - LLM_FOOTER_H;
  oledDisplay->drawFastHLine(0, y, DISPLAY_WIDTH, DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, y + 1);
  oledDisplay->print(line);
}

static void displayLLM_noModel() {
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->println("No model loaded.");
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y + LLM_LINE_H);
  oledDisplay->println("Press A to pick one,");
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y + LLM_LINE_H * 2);
  oledDisplay->println("or use /llmload.");
  drawFooter("A: pick");
}

static void displayLLM_loading() {
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->println("Loading model...");
  static const char* spin[] = {".  ", ".. ", "..."};
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y + LLM_LINE_H);
  oledDisplay->print(spin[(millis() / 300) % 3]);
  drawFooter("");
}

// Fill the content area with the loaded model's info-block icon + description, so
// a freshly loaded model shows what it is before the first question. Called only
// on the empty-conversation READY screen (there are no chat lines to draw yet).
static void drawModelCard() {
  const uint8_t* iconBits = nullptr;
  uint8_t iw = 0, ih = 0;
  const bool hasIcon = llmModelIcon(&iconBits, &iw, &ih) && iconBits;
  const char* desc = llmModelDescription();
  const bool hasDesc = desc && desc[0];
  if (!hasIcon && !hasDesc) return;  // nothing to show — leave the plain empty view

  int textX = 0;
  if (hasIcon) {
    // 1bpp MSB-first raster (matches Adafruit_GFX::drawBitmap); draw top-left.
    oledDisplay->drawBitmap(2, OLED_CONTENT_START_Y, iconBits, iw, ih, DISPLAY_COLOR_WHITE);
    textX = 2 + iw + 4;  // description sits to the right of the icon
  }

  if (!hasDesc) return;

  int charsPerLine = (DISPLAY_WIDTH - textX) / 6;
  if (charsPerLine < 1) charsPerLine = 1;
  if (charsPerLine > LLM_CHARS) charsPerLine = LLM_CHARS;

  // Greedy word-wrap the description across up to LLM_VISIBLE_LINES rows.
  const char* p = desc;
  int y = OLED_CONTENT_START_Y;
  for (int line = 0; line < LLM_VISIBLE_LINES && *p; line++) {
    int n = 0, lastSpace = -1;
    while (p[n] && n < charsPerLine) {
      if (p[n] == ' ') lastSpace = n;
      n++;
    }
    int take = n;
    if (p[n] && lastSpace > 0) take = lastSpace;  // break at a word boundary if mid-word
    char buf[LLM_CHARS + 1];
    int cp = (take < LLM_CHARS) ? take : LLM_CHARS;
    memcpy(buf, p, cp);
    buf[cp] = '\0';
    oledDisplay->setCursor(textX, y);
    oledDisplay->print(buf);
    p += take;
    while (*p == ' ') p++;  // swallow the break space(s)
    y += LLM_LINE_H;
  }
}

static void displayLLM_chat(bool generating) {
  rebuildRenderLines();

  // Clamp scroll
  int maxScroll = sRenderLineCount > LLM_VISIBLE_LINES
                  ? sRenderLineCount - LLM_VISIBLE_LINES : 0;
  if (sScrollOffset > maxScroll) sScrollOffset = maxScroll;
  if (sScrollOffset < 0) sScrollOffset = 0;

  // Empty conversation on a loaded model: show its info-block icon + description
  // instead of a blank content area. (The chat loop below draws nothing here.)
  if (!generating && chatGetTurnCount() == 0) {
    drawModelCard();
  }

  int firstVisible = sRenderLineCount - LLM_VISIBLE_LINES - sScrollOffset;
  if (firstVisible < 0) firstVisible = 0;

  int y = OLED_CONTENT_START_Y;
  for (int i = 0; i < LLM_VISIBLE_LINES; i++) {
    int idx = firstVisible + i;
    if (idx >= sRenderLineCount) break;
    oledDisplay->setCursor(0, y);
    if (sRenderLines[idx].isUserPrompt && (i == 0 || !sRenderLines[idx - 1].isUserPrompt
        || idx == 0 || sRenderLines[idx - 1].text[0] != '>')) {
      // Visual distinction: prefix with ">" if this is the first line of a user turn
      char buf[LLM_CHARS + 2];
      snprintf(buf, sizeof(buf), ">%.*s", LLM_CHARS - 1, sRenderLines[idx].text);
      oledDisplay->print(buf);
    } else {
      oledDisplay->print(sRenderLines[idx].text);
    }
    y += LLM_LINE_H;
  }

  // Streaming cursor (blink underscore at the end of the last rendered line)
  if (generating && sRenderLineCount > 0) {
    int lastVisible = firstVisible + LLM_VISIBLE_LINES - 1;
    if (lastVisible >= sRenderLineCount) lastVisible = sRenderLineCount - 1;
    if (lastVisible >= 0 && (millis() / 500) % 2 == 0) {
      int len = (int)strlen(sRenderLines[lastVisible].text);
      if (len < LLM_CHARS) {
        oledDisplay->setCursor(len * 6,
                               OLED_CONTENT_START_Y + (lastVisible - firstVisible) * LLM_LINE_H);
        oledDisplay->print("_");
      }
    }
  }

  // Scroll indicators
  if (firstVisible > 0) {
    oledDisplay->setCursor(121, OLED_CONTENT_START_Y);
    oledDisplay->print("^");
  }
  if (sScrollOffset > 0) {
    oledDisplay->setCursor(121, OLED_CONTENT_START_Y + (LLM_VISIBLE_LINES - 1) * LLM_LINE_H);
    oledDisplay->print("v");
  }

  // Footer — a PSRAM-starved context (ctx auto-fit unusably low) makes every
  // answer garbage, so when it's degraded override the key-hints with the
  // warning + the fix (restart). Same condition every other surface uses.
  int llmCtx = 0;
  if (llmContextDegraded(&llmCtx, nullptr)) {
    char f[24];
    snprintf(f, sizeof(f), "!ctx %d low-restart", llmCtx);
    drawFooter(f);
  } else if (generating) {
    drawFooter("B: stop");
  } else if (chatGetTurnCount() > 0) {
    drawFooter("A: ask  Y: retry");
  } else {
    drawFooter("A: ask");
  }
}

// Guided-menu picker views — refill the level's list, render it single-line, and
// footer the standard picker hints (or a transient busy status).
static void displayLLM_pickGroup() {
  populateGroupPicker();
  oledScrollRenderSimple(oledDisplay, &sGroupScroll);
  drawFooter(sPickStatus ? sPickStatus : "A: pick  B: back");
}

static void displayLLM_pickTemplate() {
  populateTemplatePicker();
  oledScrollRenderSimple(oledDisplay, &sTplScroll);
  drawFooter(sPickStatus ? sPickStatus : "A: pick  B: back");
}

static void displayLLM_pickEntity() {
  populateEntityPicker();
  oledScrollRenderSimple(oledDisplay, &sEntScroll);
  drawFooter(sPickStatus ? sPickStatus : "A: pick  B: back");
}

static void displayLLM() {
  if (!oledDisplay) return;
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  // Keyboard view takes over the entire surface when active
  if (sKeyboardActive && oledKeyboardIsActive()) {
    oledKeyboardDisplay(oledDisplay);
    return;
  }

  syncStateFromEngine();

  switch (sUIState) {
    case LLMUIState::NO_MODEL:      displayLLM_noModel();      break;
    case LLMUIState::LOADING:       displayLLM_loading();      break;
    case LLMUIState::READY:         displayLLM_chat(false);    break;
    case LLMUIState::GENERATING:    displayLLM_chat(true);     break;
    case LLMUIState::PICK_GROUP:    displayLLM_pickGroup();    break;
    case LLMUIState::PICK_TEMPLATE: displayLLM_pickTemplate(); break;
    case LLMUIState::PICK_ENTITY:   displayLLM_pickEntity();   break;
  }
}

// ============================================================================
// Input
// ============================================================================

static bool handleLLMInput(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  // Keyboard active: confirm/cancel are routed here; navigation handled internally.
  if (sKeyboardActive) {
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      String prompt = String(text);
      oledKeyboardReset();
      sKeyboardActive = false;
      if (prompt.length() > 0) {
        chatBeginTurn(prompt.c_str(), nullptr);
        sLastTurnGuided = false;   // free-text turn — Y retry uses chatRetryLast
      }
      return true;
    }
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sKeyboardActive = false;
      return true;
    }
    return false;
  }

  switch (sUIState) {
    case LLMUIState::NO_MODEL: {
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        // Push the shared file picker scoped to /system/llm with *.bin filter.
        // The callback (onLLMModelPicked) handles the actual model load when
        // the file browser pops back here. (X no longer aliases A — it is now the
        // guided-menu button, inert here since no model means no menu.)
        openModelPicker();
        return true;
      }
      return false;  // B falls through to global back-handler
    }

    case LLMUIState::LOADING:
      // llmLoadModel blocks the calling task during weights read, so the
      // input handler doesn't run while loading. The case is here for
      // completeness — and so a hypothetical async-load future doesn't
      // strand keys at this state.
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sUIState = LLMUIState::NO_MODEL;
        return true;
      }
      return true;

    case LLMUIState::GENERATING: {
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        chatStop();
        return true;
      }
      // Allow scroll while streaming so the user can read back without
      // stopping the generation.
      if (gNavEvents.up)   { sScrollOffset++; return true; }
      if (gNavEvents.down) { if (sScrollOffset > 0) sScrollOffset--; return true; }
      return false;
    }

    case LLMUIState::READY: {
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        oledKeyboardInit("Prompt:", nullptr, OLED_KEYBOARD_MAX_LENGTH);
        sKeyboardActive = true;
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        // X opens the guided-input menu (spec §8) — only when the loaded model
        // ships one. Otherwise X is inert (it used to alias A; that alias is gone).
        if (llmMenuGroupCount() > 0) enterPickGroup();
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
        // Retry. A guided ask must be re-run PLAIN via llmMenuRepeatLast() —
        // chatRetryLast() suppresses the previous answer's tokens, which bans the
        // memorized-correct answer for a guided question (spec §5). Branch on
        // whether our last submission was guided and the menu module still holds
        // that same session; otherwise fall back to a normal free-text retry.
        int gs = 0;
        if (sLastTurnGuided && llmMenuLastAskInfo(&gs) && gs == sGuidedAskSession) {
          // llmMenuRepeatLast advances the module's last-ask session to the new
          // turn; track it locally so a SECOND consecutive retry still matches
          // (otherwise gs != sGuidedAskSession and it falls through to
          // chatRetryLast, banning the memorized-correct answer). (spec §5)
          int sid = llmMenuRepeatLast();
          if (sid > 0) sGuidedAskSession = sid;
        } else {
          chatRetryLast(nullptr);
        }
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        return false;  // global back
      }
      if (gNavEvents.up)   { sScrollOffset++; return true; }
      if (gNavEvents.down) { if (sScrollOffset > 0) sScrollOffset--; return true; }
      return false;
    }

    case LLMUIState::PICK_GROUP: {
      if (oledScrollHandleNav(&sGroupScroll)) { sPickStatus = nullptr; return true; }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        enterPickTemplate((uint8_t)sGroupScroll.selectedIndex);
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sUIState = LLMUIState::READY;   // back one level
        sPickStatus = nullptr;
        return true;
      }
      return true;  // picker is modal — don't leak other keys to global handlers
    }

    case LLMUIState::PICK_TEMPLATE: {
      if (oledScrollHandleNav(&sTplScroll)) { sPickStatus = nullptr; return true; }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        uint16_t t = (uint16_t)sTplScroll.selectedIndex;
        sPickTpl = t;
        // Slotless (canned) templates skip the entity level and submit with e=-1;
        // a slotted template needs an entity, so descend into the entity picker.
        bool hasSlot = false;
        char probe[8];
        llmMenuTemplate(sPickGroup, t, probe, sizeof(probe), &hasSlot);
        LLMMenuGroupInfo gi;
        uint16_t entCount = llmMenuGroupInfo(sPickGroup, &gi) ? gi.entCount : 0;
        if (hasSlot && entCount > 0) enterPickEntity(t);
        else                        submitGuided(-1);
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sUIState = LLMUIState::PICK_GROUP;   // back one level
        sPickStatus = nullptr;
        return true;
      }
      return true;
    }

    case LLMUIState::PICK_ENTITY: {
      if (oledScrollHandleNav(&sEntScroll)) { sPickStatus = nullptr; return true; }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        OLEDScrollItem* sel = oledScrollGetSelected(&sEntScroll);
        if (!sel) return true;
        void* ud = sel->userData;
        if (ud == ENT_ROW_PREV) {
          sEntWindow = (sEntWindow >= LLM_ENT_PAGE) ? (uint16_t)(sEntWindow - LLM_ENT_PAGE) : 0;
          // Land on the first entity row (index 1 when a '< Prev' row precedes it).
          sEntScroll.selectedIndex = (sEntWindow > 0) ? 1 : 0;
          sEntScroll.scrollOffset  = 0;
          sPickStatus = nullptr;
        } else if (ud == ENT_ROW_NEXT) {
          uint32_t next = (uint32_t)sEntWindow + LLM_ENT_PAGE;
          if (next < sEntTotal) sEntWindow = (uint16_t)next;
          // New page always has a '< Prev' row, so the first entity is at index 1.
          sEntScroll.selectedIndex = 1;
          sEntScroll.scrollOffset  = 0;
          sPickStatus = nullptr;
        } else {
          submitGuided((int)((uintptr_t)ud) - (int)ENT_ROW_BASE);
        }
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sUIState = LLMUIState::PICK_TEMPLATE;   // back one level
        sPickStatus = nullptr;
        return true;
      }
      return true;
    }
  }
  return false;
}

// ============================================================================
// Availability / reset
// ============================================================================

static bool isLLMAvailable(String* /*outReason*/) {
  // Always available when compiled in — the surface itself shows the model
  // picker when no model is loaded, so the menu entry should stay visible
  // even on a freshly-booted device with no model selected yet.
  return true;
}

void resetLLMOLEDState() {
  chatInit();
  if (sKeyboardActive) {
    oledKeyboardReset();
    sKeyboardActive = false;
  }
  sPickStatus = nullptr;   // drop any stale guided-picker busy status
  // Snap state to whatever the engine is actually doing right now.
  LLMStatus st = llmGetStatus();
  if (chatIsGenerating())                                sUIState = LLMUIState::GENERATING;
  else if (st.state == LLMState::READY)                  sUIState = LLMUIState::READY;
  else if (st.state == LLMState::LOADING)                sUIState = LLMUIState::LOADING;
  else                                                   sUIState = LLMUIState::NO_MODEL;
  sScrollOffset = 0;
}

// ============================================================================
// Registration
// ============================================================================

// Entry hook (was an inline reset inside requestOLEDMode, guarded by
// ENABLE_ONDEVICE_LLM — now owned here, where the whole file is already gated).
static void llmOnEnter(bool /*isForward*/) {
  resetLLMOLEDState();
}

static const OLEDModeEntry sLLMModeEntry = {
  OLED_LLM,
  "LLM Chat",
  "terminal",
  displayLLM,
  isLLMAvailable,
  handleLLMInput,
  true,   // show in main menu
  95,     // menu order
  nullptr,
  llmOnEnter
};

REGISTER_OLED_MODE_MODULE(&sLLMModeEntry, 1, "LLM");

void oledLLMModeInit() {}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ONDEVICE_LLM
