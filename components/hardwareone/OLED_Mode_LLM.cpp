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

#if ENABLE_OLED_DISPLAY && ENABLE_LLM_BACKEND

#include <Adafruit_SSD1306.h>

#include "OLED_Utils.h"
#include "HAL_Input.h"          // INPUT_CHECK, INPUT_BUTTON_*, gNavEvents
#if ENABLE_LLM_SOURCE_ONBOARD
  #include "System_LLM.h"   // engine-only; the shared vocabulary is in System_LLMTypes.h
#endif
#include "System_LLMBackend.h"   // registry + source dispatch
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
  // Action menu + model picker. MENU is reached with X from READY (where X was
  // previously inert whenever the model shipped no guided menu); PICK_MODEL is
  // reached from MENU or with A from NO_MODEL. Both are list pickers over
  // OLEDScrollState, the same shape as the three guided levels below.
  MENU,
  PICK_MODEL,
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

// Action menu + model picker. sModelDescs is the selection source of truth —
// rows are display text only, and every choice is made by INDEX into this array,
// so no pointer into a rebuilt list can go stale.
#define LLM_MODEL_ROWS 12
EXT_RAM_BSS_ATTR static OLEDScrollState sMenuScroll;
EXT_RAM_BSS_ATTR static OLEDScrollState sModelScroll;
EXT_RAM_BSS_ATTR static char         sModelRows[LLM_MODEL_ROWS][LLM_CHARS + 1];
EXT_RAM_BSS_ATTR static LlmModelDesc sModelDescs[LLM_MODEL_ROWS];
static int  sModelCount = 0;
static const char* sMenuStatus = nullptr;

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

// True for every sub-state that owns the screen until the user leaves it.
// Keep in sync with the switch inside syncStateFromEngine().
static bool stateOwnsUI(LLMUIState s) {
  return s == LLMUIState::MENU        || s == LLMUIState::PICK_MODEL ||
         s == LLMUIState::PICK_GROUP  || s == LLMUIState::PICK_TEMPLATE ||
         s == LLMUIState::PICK_ENTITY;
}

// Refresh state from the engine + chat module. Called at the top of
// displayLLM() each frame so transitions land within one render tick.
static void syncStateFromEngine() {
  if (sKeyboardActive) return;  // keyboard owns the UI

  LLMStatus st = llmBackendStatus();

  // Sub-states that OWN the UI. This function is a fully-enumerated dispatcher
  // with no leave-alone default — anything that falls through to the
  // reflect-engine block at the bottom is overwritten from engine state on the
  // very next frame. So a new sub-state MUST be claimed here or it renders for
  // zero frames, and every symptom of that would be invisible in review: the
  // enum, the switch case, the populate function and the input case all look
  // correct in isolation.
  //
  // Each owning state carries its OWN exit condition; they are deliberately not
  // one shared test, because PICK_MODEL is entered precisely when NO model is
  // loaded and the guided levels require one.
  if (stateOwnsUI(sUIState)) {
    switch (sUIState) {
      case LLMUIState::PICK_MODEL:
        // No engine precondition at all — this screen is how a model gets
        // chosen in the first place. Leaves only on select or back.
        break;
      case LLMUIState::MENU:
        // Actions here act on a loaded model; if it went away, so does the menu.
        if (st.state != LLMState::READY) {
          sUIState = (st.state == LLMState::LOADING) ? LLMUIState::LOADING
                                                     : LLMUIState::NO_MODEL;
          sMenuStatus = nullptr;
        }
        break;
      default:
        // Guided levels (spec §8): an unload/swap bumps menuGeneration and an
        // unload drops the engine out of READY, either of which invalidates the
        // indices the user is picking with.
        if (st.state != LLMState::READY ||
            llmMenuGroupCount() == 0 ||
            llmMenuGeneration() != sPickGen) {
          sUIState = (st.state == LLMState::READY) ? LLMUIState::READY
                                                   : LLMUIState::NO_MODEL;
          sPickStatus = nullptr;
        }
        break;
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

// Defined with the guided pickers below; all five scroll states share one
// lazy init so the visible-line count is computed once.
static void ensurePickScrollInit();

// ============================================================================
// Model picker — a real list over the shared registry
// ============================================================================
// Was a FilePickerRequest into the generic file browser. That could only ever
// offer things that are FILES on this device, so a remote model — which has no
// path here at all — was literally unrepresentable. This is the same
// OLEDScrollState pattern the guided levels below already use.

// Fit a model name into `cap` columns, dropping a known extension and, if still
// too long, eliding the MIDDLE. Model families differ in their SUFFIX
// ("…-Q3_K_XL" vs "…-Q4_0"), so a plain head-truncation renders two different
// quantisations of one model as the same row.
static void modelDisplayName(const char* name, char* out, size_t cap) {
  if (!out || cap == 0) return;
  char base[LLM_MODEL_NAME_LEN];
  strlcpy(base, name ? name : "", sizeof(base));
  char* dot = strrchr(base, '.');
  if (dot && (strcasecmp(dot, ".gguf") == 0 || strcasecmp(dot, ".bin") == 0)) *dot = '\0';

  const size_t len = strlen(base);
  if (len < cap) { strlcpy(out, base, cap); return; }
  // head + '~' + tail
  const size_t keep = cap - 2;              // room for the '~' and the NUL
  const size_t head = (keep + 1) / 2;
  const size_t tail = keep - head;
  memcpy(out, base, head);
  out[head] = '~';
  memcpy(out + head + 1, base + len - tail, tail);
  out[head + 1 + tail] = '\0';
}

static void populateModelPicker() {
  ensurePickScrollInit();
  oledScrollClearKeepSelection(&sModelScroll);
  sModelCount = (int)llmEnumerateModels(sModelDescs, LLM_MODEL_ROWS);

  for (int i = 0; i < sModelCount; i++) {
    const LlmModelDesc& d = sModelDescs[i];
    // 21 glyphs total, and oledScrollRenderSimple prints a 2-char "> " cursor
    // first, so 19 are usable. Budget the tag first, then give the rest to the
    // name — the tag is what tells you WHERE the model runs, which matters more
    // than the last few characters of its name.
    const char* tag = (d.backend == LlmBackendKind::Cm5) ? "[pi]"
                    : (d.storage == LLM_STORAGE_SD)      ? "[sd]" : "";
    char nameBuf[LLM_CHARS + 1];
    const size_t tagLen  = strlen(tag) ? strlen(tag) + 1 : 0;   // tag + space
    const size_t markLen = d.available ? 0 : 2;                 // trailing " x"
    size_t room = 19;
    room = (room > tagLen + markLen) ? room - tagLen - markLen : 1;
    modelDisplayName(d.name, nameBuf, room + 1);
    snprintf(sModelRows[i], sizeof(sModelRows[i]), "%s%s%s%s",
             tag, tagLen ? " " : "", nameBuf, d.available ? "" : " x");
    oledScrollAddItem(&sModelScroll, sModelRows[i], nullptr, true,
                      (void*)(uintptr_t)(i + 1));   // +1 so index 0 isn't nullptr
  }
  if (sModelCount == 0) {
    strlcpy(sModelRows[0], "(no models found)", sizeof(sModelRows[0]));
    oledScrollAddItem(&sModelScroll, sModelRows[0], nullptr, false, nullptr);
  }
}

static void enterPickModel() {
  ensurePickScrollInit();
  sModelScroll.selectedIndex = 0;
  sModelScroll.scrollOffset  = 0;
  sMenuStatus = nullptr;
  populateModelPicker();
  sUIState = LLMUIState::PICK_MODEL;
}

// Commit the highlighted row. Selection is BY INDEX into sModelDescs, so a
// rebuild between frames cannot leave us holding a stale pointer.
static void commitModelPick() {
  OLEDScrollItem* sel = oledScrollGetSelected(&sModelScroll);
  if (!sel || !sel->userData) return;
  const int idx = (int)((uintptr_t)sel->userData) - 1;
  if (idx < 0 || idx >= sModelCount) return;
  if (!sModelDescs[idx].available) { sMenuStatus = "not available"; return; }

  char err[64] = {0};
  // Local selection blocks for seconds reading weights, so no LOADING frame is
  // drawn in practice; a remote selection returns immediately with the host
  // still switching, which is exactly what the LOADING state is for.
  sUIState = LLMUIState::LOADING;
  const bool ok = llmBackendSelect(sModelDescs[idx].id, err, sizeof(err));
  if (!ok) {
    sUIState = LLMUIState::PICK_MODEL;
    sMenuStatus = "load failed";
    return;
  }
  sUIState = llmBackendIsReady() ? LLMUIState::READY : LLMUIState::LOADING;
}

// ============================================================================
// Action menu (X from READY)
// ============================================================================
// X used to open the guided menu and was INERT whenever the model shipped none,
// which is most of them. Reusing it for a small action list costs no affordance
// and gives the model picker a home — the ANO encoder has only A/B/X/Y plus a
// chorded START, all already assigned, so a new button was not available.

enum : uintptr_t { MENU_ROW_ASK = 1, MENU_ROW_GUIDED, MENU_ROW_MODEL, MENU_ROW_UNLOAD };

static void populateMenuPicker() {
  ensurePickScrollInit();
  oledScrollClearKeepSelection(&sMenuScroll);
  oledScrollAddItem(&sMenuScroll, "Ask a question", nullptr, true, (void*)MENU_ROW_ASK);
  if (llmMenuGroupCount() > 0)   // 0 on every remote model, and on local ones with no MENU blob
    oledScrollAddItem(&sMenuScroll, "Guided questions", nullptr, true, (void*)MENU_ROW_GUIDED);
  oledScrollAddItem(&sMenuScroll, "Switch model",   nullptr, true, (void*)MENU_ROW_MODEL);
  oledScrollAddItem(&sMenuScroll, "Unload model",   nullptr, true, (void*)MENU_ROW_UNLOAD);
}

static void enterMenu() {
  ensurePickScrollInit();
  sMenuScroll.selectedIndex = 0;
  sMenuScroll.scrollOffset  = 0;
  sMenuStatus = nullptr;
  populateMenuPicker();
  sUIState = LLMUIState::MENU;
}

// ============================================================================
// Guided-input menu pickers — build/refill lists from the shared C API
// ============================================================================

static void ensurePickScrollInit() {
  if (sPickScrollInit) return;
  int vis = OLED_CONTENT_HEIGHT / 8;   // single-line (8px) rows in the content area
  if (vis < 1) vis = 1;
  oledScrollInit(&sMenuScroll,  nullptr, vis);
  oledScrollInit(&sModelScroll, nullptr, vis);
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
    // Fold to the ASCII the classic GFX font can actually draw, BEFORE counting
    // lines. This pass and the emit pass below must fold identically or the
    // line count and the emitted text disagree and the view scrolls wrong.
    //
    // Folding is what makes the byte arithmetic below correct: post-fold one
    // byte is one glyph, so ceil(copied / LLM_CHARS) is a real line count and
    // the memcpy slice is a real 21-glyph line. Un-folded, a multi-byte
    // character both drew as garbage AND stole glyphs from the line.
    copied = (int)utf8FoldToAscii(turnBuf, (size_t)copied);
    turnBuf[copied] = '\0';
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
    // Must match the fold in the counting pass above exactly.
    copied = (int)utf8FoldToAscii(turnBuf, (size_t)copied);
    turnBuf[copied] = '\0';

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

static void displayLLM_menu() {
  populateMenuPicker();
  oledScrollRenderSimple(oledDisplay, &sMenuScroll);
  drawFooter(sMenuStatus ? sMenuStatus : "A: pick  B: back");
}

static void displayLLM_pickModel() {
  populateModelPicker();
  oledScrollRenderSimple(oledDisplay, &sModelScroll);
  drawFooter(sMenuStatus ? sMenuStatus : "A: load  B: back");
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
    case LLMUIState::MENU:          displayLLM_menu();         break;
    case LLMUIState::PICK_MODEL:    displayLLM_pickModel();    break;
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
        // A real list over every source, not a file browser: a remote model has
        // no file on this device and cannot be represented as one.
        enterPickModel();
        return true;
      }
      return false;  // B falls through to global back-handler
    }

    case LLMUIState::LOADING:
      // The "hypothetical async-load future" this case was written for is here:
      // a CM5-routed select returns immediately and the host may take tens of
      // seconds to restart llama-server, so this is now a long-lived state that
      // the input handler DOES run in.
      //
      // The old body could not escape it. Setting NO_MODEL achieved nothing —
      // syncStateFromEngine re-derives LOADING from the engine on the very next
      // frame — and returning true swallowed B, so the central dispatcher's
      // "handler declined and B was pressed" path never ran oledMenuBack() and
      // the user could not leave the mode at all short of the global Quick
      // Settings key. Decline B so that back-handler fires.
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) return false;
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
        oledKeyboardInit("Prompt:", nullptr, OLED_KEYBOARD_MAX_LENGTH,
                         OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT);
        sKeyboardActive = true;
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        // X opens the action menu, which is where guided questions now live
        // alongside Switch model / Unload. Previously X went straight to the
        // guided picker and was inert whenever the model shipped no menu.
        enterMenu();
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

    case LLMUIState::MENU: {
      if (oledScrollHandleNav(&sMenuScroll)) { sMenuStatus = nullptr; return true; }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        OLEDScrollItem* sel = oledScrollGetSelected(&sMenuScroll);
        if (!sel) return true;
        switch ((uintptr_t)sel->userData) {
          case MENU_ROW_ASK:
            oledKeyboardInit("Prompt:", nullptr, OLED_KEYBOARD_MAX_LENGTH,
                             OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT);
            sKeyboardActive = true;
            sUIState = LLMUIState::READY;   // keyboard overlay owns the screen
            break;
          case MENU_ROW_GUIDED:
            if (llmMenuGroupCount() > 0) enterPickGroup();
            break;
          case MENU_ROW_MODEL:
            enterPickModel();
            break;
          case MENU_ROW_UNLOAD:
            llmBackendUnload();
            sUIState = LLMUIState::NO_MODEL;
            break;
          default: break;
        }
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sUIState = LLMUIState::READY;
        sMenuStatus = nullptr;
        return true;
      }
      return true;   // modal — don't leak keys to the global handlers
    }

    case LLMUIState::PICK_MODEL: {
      if (oledScrollHandleNav(&sModelScroll)) { sMenuStatus = nullptr; return true; }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        commitModelPick();
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        // Back goes wherever we came from: the chat view if a model is already
        // live, otherwise the empty-state screen.
        sUIState = llmBackendIsReady() ? LLMUIState::READY : LLMUIState::NO_MODEL;
        sMenuStatus = nullptr;
        return true;
      }
      return true;
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
  LLMStatus st = llmBackendStatus();
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
// ENABLE_LLM_SOURCE_ONBOARD — now owned here, where the whole file is already gated).
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

#endif // ENABLE_OLED_DISPLAY && ENABLE_LLM_BACKEND
