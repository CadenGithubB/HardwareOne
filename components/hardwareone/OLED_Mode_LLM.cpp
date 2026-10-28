// ============================================================================
// OLED LLM Chat Mode
// ============================================================================
// State-machine driven chat surface backed by System_LLMChat (the shared
// conversation module). It works like the glasses' LLM app: it opens on a
// home menu whose rows come from System_LLMHomeMenuCore.h, the list the G2
// builds too (Open chat / Ask (Mic / Keys) / Ask (guided) / New chat /
// Re-run last / Model, plus a low-context warning). The chat view:
//
//   NO_MODEL    — no model is loaded. A opens the model picker.
//   LOADING     — model load in progress (`llmload` runs on cmd_exec; this
//                 screen holds until its reply arrives and the engine settles).
//   READY       — the latest question and its whole reply, opened at the first
//                 line like the glasses' reader. Up/down scroll a line,
//                 left/right a screen; A asks, Y re-runs, B or X returns home.
//   GENERATING  — the same view while the reply arrives; B stops it.
//
// The model picker lists every source's models through the backend registry,
// with "Unload model" first, as on the glasses. Footer hints go through the
// shared footer (oledLLMFooterHint), which owns the bottom of the screen.
//
//   - The reader re-reads the latest run (chatReadRun) only when it changes and
//     wraps it with the shared System_TextPager core; turns stay in the chat
//     module.
//   - No private xTaskCreate'd worker; generation rides the engine's existing
//     async task and we pull-on-read each render.
//   - Settings actually apply (gSettings.llm* is consumed by chatBeginTurn).

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
#include "System_TextPager.h"     // textWrapAppend — the shared wrap core
#include "System_Debug.h"
#include "System_MemUtil.h"

// ============================================================================
// Layout constants
// ============================================================================

static const int LLM_CHARS         = 21;   // Characters per line at text size 1
static const int LLM_LINE_H        = 10;   // Pixels per text line
// Lines between the header and the shared footer: 4 on the 128x64 OLED, 9 on
// the P4X-EYE's 128x128 canvas.
static const int LLM_VISIBLE_LINES = OLED_CONTENT_HEIGHT / LLM_LINE_H > 0
                                     ? OLED_CONTENT_HEIGHT / LLM_LINE_H : 1;
// The reply reader wraps one column short so the scroll marks in the last
// column never cover text.
static const int LLM_READER_COLS   = LLM_CHARS - 1;

// ============================================================================
// UI state
// ============================================================================

enum class LLMUIState : uint8_t {
  NO_MODEL = 0,
  LOADING,
  READY,
  GENERATING,
  // Home menu + model picker, both list pickers over OLEDScrollState, the same
  // shape as the three guided levels below. HOME is where the mode opens and
  // where B returns from the chat view and the pickers; PICK_MODEL is reached
  // from HOME's Model row or with A from NO_MODEL.
  HOME,
  PICK_MODEL,
  // Guided-input menu pickers (LLM_GUIDED_MENU_SPEC §8) — nested sub-states
  // entered from HOME's "Ask (guided)" row when the loaded model ships a menu.
  // Back (B) pops one level: ENTITY -> TEMPLATE -> GROUP -> HOME.
  PICK_GROUP,
  PICK_TEMPLATE,
  PICK_ENTITY,
};

static LLMUIState sUIState = LLMUIState::HOME;
static bool       sKeyboardActive = false;
static LLMUIState sKeyboardReturn = LLMUIState::HOME;  // where a cancelled or empty prompt goes
static bool       sLoadReturnsHome = false; // a picker load settles on HOME, not the chat view

// The shared footer (drawOLEDFooter) clears and draws the bottom strip after
// this mode renders, so the mode hands it its hint instead of drawing one.
static char sFooterHint[LLM_CHARS + 1] = "";

// Reply reader, as on the glasses: the latest exchange (the question, then the
// whole reply), wrapped once whenever the reply changes and scrolled by line.
// The buffers are PSRAM; a remote reply can be 16 KiB.
#define LLM_READER_MAX_LINES 1024
static const size_t LLM_READER_WRAP_CAP =
    LLM_CHAT_TURN_MAX_BYTES + LLM_CHAT_TURN_MAX_BYTES / LLM_READER_COLS + 256;
static_assert(LLM_READER_WRAP_CAP <= UINT16_MAX, "line offsets are uint16_t");
EXT_RAM_BSS_ATTR static char     sReplyRaw[LLM_CHAT_TURN_MAX_BYTES];
EXT_RAM_BSS_ATTR static char     sReplyWrapped[LLM_READER_WRAP_CAP];
EXT_RAM_BSS_ATTR static uint16_t sReplyLineOff[LLM_READER_MAX_LINES];
static int          sReplyLines  = 0;      // lines in sReplyWrapped
static ChatRunToken sReplyRun    = 0;      // the run the buffers hold
static int          sReplyRawLen = 0;      // raw bytes read from that run
static bool         sReplyDone   = false;  // that run had finished at the last read
static bool         sReplyStale  = true;   // rebuild on the next frame
static int          sReaderTop   = 0;      // first visible line

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

// Home menu + model picker. sModelDescs is the selection source of truth —
// rows are display text only, and every choice is made by INDEX into this array,
// so no pointer into a rebuilt list can go stale.
#define LLM_MODEL_ROWS 12
#define LLM_HOME_ROWS  8
EXT_RAM_BSS_ATTR static OLEDScrollState sHomeScroll;
EXT_RAM_BSS_ATTR static llmhome::Row    sHomeRows[LLM_HOME_ROWS];
EXT_RAM_BSS_ATTR static OLEDScrollState sModelScroll;
EXT_RAM_BSS_ATTR static char         sModelRows[LLM_MODEL_ROWS][LLM_CHARS + 1];
EXT_RAM_BSS_ATTR static LlmModelDesc sModelDescs[LLM_MODEL_ROWS];
static int  sModelCount = 0;
static const char* sMenuStatus = nullptr;
static char sMenuStatusBuffer[64];

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

static void clearChatForOled() {
  if (!chatClear()) {
    sMenuStatus = "Stop/wait, then retry";
    return;
  }
  sLastTurnGuided = false;
  sGuidedAskSession = 0;
  sReplyStale = true;
  sMenuStatus = "New chat ready";
}

// ============================================================================
// Load / unload through the command executor
// ============================================================================
// `llmload` and `llmunload` are admin commands. Submitting them under the
// display identity applies the same role check as every other surface, and the
// load runs on cmd_exec, serialised with loads and unloads from the CLI, web
// and G2, instead of blocking this loop for seconds. One request at a time; the
// reply callback runs on cmd_exec and only fills this mailbox, which the
// render loop drains.
enum : uint8_t { LLM_CMD_NONE = 0, LLM_CMD_LOAD, LLM_CMD_UNLOAD };
static uint8_t sCmdInFlight = LLM_CMD_NONE;          // owner task only
static portMUX_TYPE sCmdMux = portMUX_INITIALIZER_UNLOCKED;
static bool        sCmdReplyReady  = false;          // guarded by sCmdMux
static uint8_t     sCmdReplyOp     = LLM_CMD_NONE;   // guarded by sCmdMux
static const char* sCmdReplyStatus = nullptr;        // guarded by sCmdMux; literal, nullptr = success
static char        sCmdReplyReason[sizeof(sMenuStatusBuffer)];  // guarded by sCmdMux; the reply's "error", or empty

static void llmCommandDone(bool ok, const char* result, void* userData) {
  const uint8_t op = (uint8_t)(uintptr_t)userData;
  const char* r = result ? result : "";
  const char* status = nullptr;
  char reason[sizeof(sCmdReplyReason)] = {0};
  if (strstr(r, "Admin access")) {
    status = "admin required";
  } else if (!ok || strncmp(r, "Error", 5) == 0 || strstr(r, "\"ok\":false")) {
    status = (op == LLM_CMD_LOAD) ? "load failed" : "unload failed";
    // The backend's reason (a remote host's message, a missing model) is the
    // reply's "error" string, which the firmware writes without quotes.
    const char* e = strstr(r, "\"error\":\"");
    if (e) {
      e += 9;
      size_t n = 0;
      while (e[n] && e[n] != '"' && n + 1 < sizeof(reason)) { reason[n] = e[n]; ++n; }
      reason[n] = '\0';
    }
  }
  portENTER_CRITICAL(&sCmdMux);
  sCmdReplyOp = op;
  sCmdReplyStatus = status;
  memcpy(sCmdReplyReason, reason, sizeof(reason));
  sCmdReplyReady = true;
  portEXIT_CRITICAL(&sCmdMux);
}

static bool submitLlmCommand(const char* line, uint8_t op) {
  if (sCmdInFlight != LLM_CMD_NONE) return false;
  String user;
  bool authed = false;
  const TransportSessionEpoch epoch = localDisplayTransportSessionSnapshot(user, authed);
  if (epoch == kNoTransportSessionEpoch) return false;
  if (!submitOLEDCommandForSession(String(line), epoch, llmCommandDone,
                                   (void*)(uintptr_t)op)) {
    return false;
  }
  sCmdInFlight = op;
  return true;
}

// A refused or failed request returns to the screen it came from with the
// reason on the status line; a success leaves the engine state to drive.
static void pollLlmCommandReply() {
  if (sCmdInFlight == LLM_CMD_NONE) return;
  bool ready = false;
  uint8_t op = LLM_CMD_NONE;
  const char* status = nullptr;
  char reason[sizeof(sCmdReplyReason)];
  portENTER_CRITICAL(&sCmdMux);
  if (sCmdReplyReady) {
    ready = true;
    op = sCmdReplyOp;
    status = sCmdReplyStatus;
    memcpy(reason, sCmdReplyReason, sizeof(reason));
    sCmdReplyReady = false;
  }
  portEXIT_CRITICAL(&sCmdMux);
  if (!ready) return;
  sCmdInFlight = LLM_CMD_NONE;
  if (op == LLM_CMD_UNLOAD) {
    // Unload starts in the model picker and lands on HOME, whose Model row
    // follows the engine; only the outcome is left to say.
    if (!status) {
      sMenuStatus = "Model unloaded";
    } else if (reason[0]) {
      strlcpy(sMenuStatusBuffer, reason, sizeof(sMenuStatusBuffer));
      sMenuStatus = sMenuStatusBuffer;
    } else {
      sMenuStatus = status;
    }
    return;
  }
  if (!status) return;
  if (op == LLM_CMD_LOAD && sUIState == LLMUIState::LOADING) {
    sUIState = LLMUIState::PICK_MODEL;
    sLoadReturnsHome = false;
    if (reason[0]) {
      strlcpy(sMenuStatusBuffer, reason, sizeof(sMenuStatusBuffer));
      sMenuStatus = sMenuStatusBuffer;
    } else {
      sMenuStatus = status;
    }
  }
}

// ============================================================================
// State transitions
// ============================================================================

// True for every sub-state that owns the screen until the user leaves it.
// Keep in sync with the switch inside syncStateFromEngine().
static bool stateOwnsUI(LLMUIState s) {
  return s == LLMUIState::HOME        || s == LLMUIState::PICK_MODEL ||
         s == LLMUIState::PICK_GROUP  || s == LLMUIState::PICK_TEMPLATE ||
         s == LLMUIState::PICK_ENTITY;
}

// Refresh state from the engine + chat module. Called at the top of
// displayLLM() each frame so transitions land within one render tick.
static void syncStateFromEngine() {
  pollLlmCommandReply();
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
      case LLMUIState::HOME:
      case LLMUIState::PICK_MODEL:
        // No engine precondition: HOME's rows are rebuilt from the engine every
        // frame, and the picker is how a model gets chosen in the first place.
        // Both leave only on a selection or back.
        break;
      default:
        // Guided levels (spec §8): an unload/swap bumps menuGeneration and an
        // unload drops the engine out of READY, either of which invalidates the
        // indices the user is picking with.
        if (st.state != LLMState::READY ||
            llmMenuGroupCount() == 0 ||
            llmMenuGeneration() != sPickGen) {
          sUIState = LLMUIState::HOME;
          sPickStatus = nullptr;
        }
        break;
    }
    return;
  }

  // Generation overrides everything else
  if (chatIsGenerating()) {
    if (sUIState != LLMUIState::GENERATING) {
      sUIState = LLMUIState::GENERATING;
    }
    return;
  }

  // From GENERATING → settle to READY when chat says it's done
  if (sUIState == LLMUIState::GENERATING) {
    sUIState = LLMUIState::READY;
    return;
  }

  // From LOADING → READY/NO_MODEL based on engine state. While our own
  // `llmload` is queued or running, the engine still shows the previous model
  // (or none), so hold until its reply arrives.
  // A load started from the picker settles on HOME, as on the glasses, where
  // the Model row now names it. A load the chat view only watched (another
  // surface started it) settles back on the chat view.
  if (sUIState == LLMUIState::LOADING) {
    if (sCmdInFlight == LLM_CMD_LOAD) return;
    if (st.state == LLMState::READY ||
        st.state == LLMState::ERROR || st.state == LLMState::UNLOADED) {
      if (sLoadReturnsHome) {
        sUIState = LLMUIState::HOME;
        sLoadReturnsHome = false;
      } else {
        sUIState = (st.state == LLMState::READY) ? LLMUIState::READY : LLMUIState::NO_MODEL;
      }
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

// Model rows carry (void*)(index + 1); the Unload row sits past every index.
static void* const MODEL_ROW_UNLOAD = (void*)(uintptr_t)(LLM_MODEL_ROWS + 1);

// "Unload model" first, then every source's models, with the loaded one marked
// by a leading '*' — the glasses' picker in the same order.
static void populateModelPicker() {
  ensurePickScrollInit();
  oledScrollClearKeepSelection(&sModelScroll);
  sModelCount = (int)llmEnumerateModels(sModelDescs, LLM_MODEL_ROWS);
  oledScrollAddItem(&sModelScroll, "Unload model", nullptr, true, MODEL_ROW_UNLOAD);

  // Compare IDs, not paths: a remote model has no path.
  LlmModelDesc active;
  const bool haveActive = llmBackendIsReady() && llmBackendActiveModel(&active);
  for (int i = 0; i < sModelCount; i++) {
    const LlmModelDesc& d = sModelDescs[i];
    // 21 glyphs total, and oledScrollRenderSimple prints a 2-char "> " cursor
    // first, so 19 are usable. Budget the tag first, then give the rest to the
    // name — the tag is what tells you WHERE the model runs, which matters more
    // than the last few characters of its name.
    const char* tag = (d.backend == LlmBackendKind::Cm5) ? "[pi]"
                    : (d.storage == LLM_STORAGE_SD)      ? "[sd]" : "";
    const bool isActive = haveActive && strcmp(d.id, active.id) == 0;
    char nameBuf[LLM_CHARS + 1];
    const size_t tagLen  = strlen(tag) ? strlen(tag) + 1 : 0;   // tag + space
    const size_t markLen = (d.available ? 0 : 2) + (isActive ? 1 : 0);  // " x", "*"
    size_t room = 19;
    room = (room > tagLen + markLen) ? room - tagLen - markLen : 1;
    llmhome::fitName(d.name, room, nameBuf, sizeof(nameBuf));
    snprintf(sModelRows[i], sizeof(sModelRows[i]), "%s%s%s%s%s", isActive ? "*" : "",
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
  if (sel->userData == MODEL_ROW_UNLOAD) {
    if (sCmdInFlight != LLM_CMD_NONE) { sMenuStatus = "busy"; return; }
    if (!submitLlmCommand("llmunload", LLM_CMD_UNLOAD)) { sMenuStatus = "unload failed"; return; }
    sMenuStatus = "Unloading...";   // pollLlmCommandReply replaces it with the outcome
    sUIState = LLMUIState::HOME;
    return;
  }
  const int idx = (int)((uintptr_t)sel->userData) - 1;
  if (idx < 0 || idx >= sModelCount) return;
  if (!sModelDescs[idx].available) { sMenuStatus = "not available"; return; }

  if (sCmdInFlight != LLM_CMD_NONE) { sMenuStatus = "busy"; return; }
  // The id is copied into the command line now, so a picker rebuild while the
  // command waits on cmd_exec cannot change what loads. LOADING holds until
  // the reply; a refusal or failure comes back here with the reason.
  char cmd[LLM_MODEL_ID_LEN + 24];
  if (!llmBuildLoadCommand(sModelDescs[idx].id, 0, cmd, sizeof(cmd)) ||
      !submitLlmCommand(cmd, LLM_CMD_LOAD)) {
    sMenuStatus = "load failed";
    return;
  }
  sMenuStatus = nullptr;
  sLoadReturnsHome = true;
  sUIState = LLMUIState::LOADING;
}

// ============================================================================
// Home menu — the rows the glasses show too (System_LLMHomeMenuCore.h)
// ============================================================================
// Rebuilt every frame so the Model row and the optional rows (guided, re-run,
// low context) follow the engine. Each row's action rides in its userData,
// offset by one so no row carries nullptr.

static void populateHomeMenu() {
  ensurePickScrollInit();
  oledScrollClearKeepSelection(&sHomeScroll);
  const size_t n = llmHomeMenuRowsNow(false, LLM_CHARS - 2, sHomeRows, LLM_HOME_ROWS);
  for (size_t i = 0; i < n; i++) {
    oledScrollAddItem(&sHomeScroll, sHomeRows[i].label, nullptr, true,
                      (void*)(uintptr_t)((uint8_t)sHomeRows[i].action + 1));
  }
  oledScrollClampSelection(&sHomeScroll);
}

static void enterHome() {
  ensurePickScrollInit();
  sPickStatus = nullptr;
  sUIState = LLMUIState::HOME;
}

// ============================================================================
// Guided-input menu pickers — build/refill lists from the shared C API
// ============================================================================

static void ensurePickScrollInit() {
  if (sPickScrollInit) return;
  int vis = OLED_CONTENT_HEIGHT / 8;   // single-line (8px) rows in the content area
  if (vis < 1) vis = 1;
  oledScrollInit(&sHomeScroll,  nullptr, vis);
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
    sUIState          = LLMUIState::GENERATING;   // stream the answer (existing view)
  } else if (rc == 0) {
    // Busy — a generation is already in flight. Stay in the picker; show status.
    sPickStatus = "Busy: answer in progress";
  } else {
    // -1 stale gen (model swapped) / -2 bad index / -3 no menu — back to HOME,
    // whose rows show what the engine has now.
    sPickStatus = nullptr;
    sUIState    = LLMUIState::HOME;
  }
}

// ============================================================================
// Reply reader — the latest exchange, wrapped once per change (as on the G2)
// ============================================================================

// Fold to the ASCII the classic GFX font can draw, one line at a time:
// utf8FoldToAscii turns control bytes, newlines included, into spaces, and the
// reader keeps the reply's line breaks. Folding never grows text, so it runs in
// place. Returns the new length.
static size_t foldKeepingNewlines(char* s, size_t n) {
  size_t w = 0, start = 0;
  for (size_t i = 0; i <= n; ++i) {
    if (i < n && s[i] != '\n') continue;
    const size_t segment = i - start;
    memmove(s + w, s + start, segment);
    w += utf8FoldToAscii(s + w, segment);
    if (i < n) s[w++] = '\n';
    start = i + 1;
  }
  s[w] = '\0';
  return w;
}

// Re-read the latest run and its question and wrap them. A new run opens at its
// first line, as the glasses' reader opens a new reply on page one.
static void rebuildReader(ChatRunToken run) {
  if (run != sReplyRun) sReaderTop = 0;
  sReplyRun = run;
  sReplyStale = false;
  bool done = false;
  int len = run ? chatReadRun(run, 0, sReplyRaw, (int)sizeof(sReplyRaw), &done) : 0;
  if (len < 0) { len = 0; done = true; }   // cleared or replaced since the token was read
  sReplyRawLen = len;
  sReplyDone = done;

  size_t pos = 0;
  bool truncated = false;
  sReplyWrapped[0] = '\0';
  sReplyLines = 0;
  if (!run) return;

  // The question: the newest user turn, up to two lines.
  for (int i = chatGetTurnCount() - 1; i >= 0; --i) {
    ChatTurnInfo info;
    if (!chatGetTurnInfo(i, &info) || info.role != ChatTurnRole::USER) continue;
    // "> " then two rows: 18 glyphs on the first, 18 after the 2-space indent.
    char question[2 + (LLM_READER_COLS - 2) * 2 + 1] = "> ";
    const int n = chatReadTurn(i, 0, question + 2, (int)sizeof(question) - 2);
    if (n > 0) {
      question[2 + utf8FoldToAscii(question + 2, (size_t)n)] = '\0';
      textWrapAppend(sReplyWrapped, sizeof(sReplyWrapped), pos, question,
                     LLM_READER_COLS, 2, true, &truncated);
      textWrapAppend(sReplyWrapped, sizeof(sReplyWrapped), pos, "\n",
                     LLM_READER_COLS, 0, true, &truncated);
    }
    break;
  }

  const size_t folded = foldKeepingNewlines(sReplyRaw, (size_t)len);
  const char* reply = folded ? sReplyRaw : done ? "(empty reply)" : "Waiting for reply...";
  textWrapAppend(sReplyWrapped, sizeof(sReplyWrapped), pos, reply,
                 LLM_READER_COLS, 0, true, &truncated);
  while (pos > 0 && sReplyWrapped[pos - 1] == '\n') sReplyWrapped[--pos] = '\0';

  if (pos == 0) return;
  sReplyLineOff[sReplyLines++] = 0;
  for (size_t i = 0; i < pos && sReplyLines < LLM_READER_MAX_LINES; ++i) {
    if (sReplyWrapped[i] == '\n') sReplyLineOff[sReplyLines++] = (uint16_t)(i + 1);
  }
}

// Called every frame. Cheap unless the reply changed: a finished run is left
// alone, and a live one is probed for one byte past what was read.
static void syncReader() {
  const ChatRunToken run = chatGetRunToken();
  if (sReplyStale || run != sReplyRun) { rebuildReader(run); return; }
  if (!run || sReplyDone) return;
  char probe[2];
  bool done = false;
  const int n = chatReadRun(run, sReplyRawLen, probe, (int)sizeof(probe), &done);
  if (n != 0 || done) rebuildReader(run);   // more text, finished, or replaced (-1)
}

// Line `index` of the wrapped reply, copied into `out` (at most one row).
static void readerLine(int index, char* out, size_t cap) {
  out[0] = '\0';
  if (index < 0 || index >= sReplyLines || cap == 0) return;
  const char* p = sReplyWrapped + sReplyLineOff[index];
  size_t n = 0;
  while (p[n] && p[n] != '\n' && n + 1 < cap) { out[n] = p[n]; ++n; }
  out[n] = '\0';
}

static void scrollReader(int lines) {
  const int maxTop = sReplyLines > LLM_VISIBLE_LINES ? sReplyLines - LLM_VISIBLE_LINES : 0;
  sReaderTop += lines;
  if (sReaderTop > maxTop) sReaderTop = maxTop;
  if (sReaderTop < 0) sReaderTop = 0;
}

// ============================================================================
// Display
// ============================================================================

static void drawFooter(const char* line) {
  strlcpy(sFooterHint, line ? line : "", sizeof(sFooterHint));
}

// The hint drawOLEDFooter() shows for this mode: whatever the last frame set.
const char* oledLLMFooterHint() {
  return sFooterHint;
}

static void displayLLM_noModel() {
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->println("No model loaded.");
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y + LLM_LINE_H);
  oledDisplay->println("Press A to pick one,");
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y + LLM_LINE_H * 2);
  oledDisplay->println("or use /llmload.");
  drawFooter("A:pick  B:menu");
}

static void displayLLM_loading() {
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->println("Loading model...");
  static const char* spin[] = {".  ", ".. ", "..."};
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y + LLM_LINE_H);
  oledDisplay->print(spin[(millis() / 300) % 3]);
  drawFooter("B:menu");
}

// Fill the content area with the loaded model's info-block icon + description, so
// a freshly loaded model shows what it is before the first question. Called only
// on the empty-conversation READY screen (there are no chat lines to draw yet).
// Returns false when the model has neither (every remote source).
static bool drawModelCard() {
  const uint8_t* iconBits = nullptr;
  uint8_t iw = 0, ih = 0;
  const bool hasIcon = llmModelIcon(&iconBits, &iw, &ih) && iconBits;
  const char* desc = llmModelDescription();
  const bool hasDesc = desc && desc[0];
  if (!hasIcon && !hasDesc) return false;

  int textX = 0;
  if (hasIcon) {
    // 1bpp MSB-first raster (matches Adafruit_GFX::drawBitmap); draw top-left.
    oledDisplay->drawBitmap(2, OLED_CONTENT_START_Y, iconBits, iw, ih, DISPLAY_COLOR_WHITE);
    textX = 2 + iw + 4;  // description sits to the right of the icon
  }

  if (!hasDesc) return true;

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
  return true;
}

static void displayLLM_chat(bool generating) {
  syncReader();

  if (sReplyLines == 0) {
    // No exchange yet: the loaded model's icon + description, or, for a model
    // without one, how to start.
    if (!drawModelCard()) {
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      oledDisplay->print("No messages yet.");
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y + LLM_LINE_H);
      oledDisplay->print("Press A to ask.");
    }
  } else {
    scrollReader(0);   // clamp after a rebuild shortened the reply
    char line[LLM_CHARS + 1];
    for (int i = 0; i < LLM_VISIBLE_LINES; i++) {
      readerLine(sReaderTop + i, line, sizeof(line));
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y + i * LLM_LINE_H);
      oledDisplay->print(line);
    }
    // Scroll marks in the column the reader leaves free.
    const int markX = LLM_READER_COLS * 6 + 2;
    if (sReaderTop > 0) {
      oledDisplay->setCursor(markX, OLED_CONTENT_START_Y);
      oledDisplay->print("^");
    }
    if (sReaderTop + LLM_VISIBLE_LINES < sReplyLines) {
      oledDisplay->setCursor(markX, OLED_CONTENT_START_Y + (LLM_VISIBLE_LINES - 1) * LLM_LINE_H);
      oledDisplay->print("v");
    }
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
  } else if (chatGetTurnCount() > 0 && llmBackendSupportsRetry()) {
    drawFooter("A:ask Y:retry B:menu");
  } else {
    drawFooter("A:ask  B:menu");
  }
}

static void displayLLM_home() {
  populateHomeMenu();
  oledScrollRenderSimple(oledDisplay, &sHomeScroll);
  drawFooter(sMenuStatus ? sMenuStatus : "A:pick  B:back");
}

static void displayLLM_pickModel() {
  populateModelPicker();
  oledScrollRenderSimple(oledDisplay, &sModelScroll);
  drawFooter(sMenuStatus ? sMenuStatus : "A:load Y:new B:back");
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

// The OLED redraws only a screen marked dirty. While something here changes on
// its own (a reply arriving, a model loading, a load or unload command out),
// the screen asks for its next frame, as the Speech screen does; without that a
// reply was never drawn until a key was pressed. Idle, it waits for input.
static bool llmScreenIsLive() {
  return sUIState == LLMUIState::GENERATING || sUIState == LLMUIState::LOADING ||
         sCmdInFlight != LLM_CMD_NONE || (sReplyRun != 0 && !sReplyDone);
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
    case LLMUIState::HOME:          displayLLM_home();         break;
    case LLMUIState::PICK_MODEL:    displayLLM_pickModel();    break;
    case LLMUIState::PICK_GROUP:    displayLLM_pickGroup();    break;
    case LLMUIState::PICK_TEMPLATE: displayLLM_pickTemplate(); break;
    case LLMUIState::PICK_ENTITY:   displayLLM_pickEntity();   break;
  }
  if (llmScreenIsLive()) oledMarkDirty();
}

// ============================================================================
// Input
// ============================================================================

// Opens the prompt keyboard (it can also dictate); a cancelled or empty prompt
// returns to `from`, a submitted one to the chat view to watch the answer.
static void openPrompt(LLMUIState from) {
  oledKeyboardInit("Prompt:", nullptr, OLED_KEYBOARD_MAX_LENGTH,
                   OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT);
  sKeyboardActive = true;
  sKeyboardReturn = from;
}

// Re-run the last question. A guided ask must be re-run PLAIN via
// llmMenuRepeatLast() — chatRetryLast() suppresses the previous answer's tokens,
// which bans the memorized-correct answer for a guided question (spec §5).
// Branch on whether our last submission was guided and the menu module still
// holds that same session; otherwise fall back to a normal free-text retry.
static void rerunLast() {
  if (!llmBackendSupportsRetry()) return;
  int gs = 0;
  if (sLastTurnGuided && llmMenuLastAskInfo(&gs) && gs == sGuidedAskSession) {
    // llmMenuRepeatLast advances the module's last-ask session to the new turn;
    // track it locally so a SECOND consecutive retry still matches (otherwise
    // gs != sGuidedAskSession and it falls through to chatRetryLast, banning
    // the memorized-correct answer). (spec §5)
    int sid = llmMenuRepeatLast();
    if (sid > 0) sGuidedAskSession = sid;
  } else {
    chatRetryLast(nullptr);
  }
}

// Up/down move the reply one line, left/right a screen (keeping one line of
// context), as the glasses page through a reply.
static bool scrollReaderByNav() {
  const int page = LLM_VISIBLE_LINES > 1 ? LLM_VISIBLE_LINES - 1 : 1;
  if (gNavEvents.up)    { scrollReader(-1);    return true; }
  if (gNavEvents.down)  { scrollReader(1);     return true; }
  if (gNavEvents.left)  { scrollReader(-page); return true; }
  if (gNavEvents.right) { scrollReader(page);  return true; }
  return false;
}

// The chat view; the next sync settles it on GENERATING, LOADING or NO_MODEL
// when the engine says so.
static void openChatView() {
  sReplyStale = true;   // reread, and open at the first line as the glasses do
  sReaderTop = 0;
  sMenuStatus = nullptr;
  sUIState = LLMUIState::READY;
}

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
        openChatView();
      } else {
        sUIState = sKeyboardReturn;
      }
      return true;
    }
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sKeyboardActive = false;
      sUIState = sKeyboardReturn;
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
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) { enterHome(); return true; }
      return false;
    }

    case LLMUIState::LOADING:
      // A CM5-routed select returns immediately and the host may take tens of
      // seconds to restart llama-server, so this is a long-lived state. B goes
      // home while the load carries on; HOME's Model row shows when it lands.
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sLoadReturnsHome = false;
        enterHome();
      }
      return true;

    case LLMUIState::GENERATING: {
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        chatStop();
        return true;
      }
      // Allow scroll while streaming so the user can read back without
      // stopping the generation.
      if (scrollReaderByNav()) return true;
      return false;
    }

    case LLMUIState::READY: {
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
        openPrompt(LLMUIState::READY);
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
        rerunLast();
        return true;
      }
      // B returns home, as the glasses' viewer does; X does too, where it used
      // to open the action menu that HOME replaced.
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        enterHome();
        return true;
      }
      if (scrollReaderByNav()) return true;
      return false;
    }

    case LLMUIState::HOME: {
      if (oledScrollHandleNav(&sHomeScroll)) { sMenuStatus = nullptr; return true; }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        OLEDScrollItem* sel = oledScrollGetSelected(&sHomeScroll);
        if (!sel || !sel->userData) return true;
        sMenuStatus = nullptr;
        switch ((LlmHomeAction)((uintptr_t)sel->userData - 1)) {
          case LlmHomeAction::Open:
            openChatView();
            break;
          case LlmHomeAction::Ask:
            if (llmBackendIsReady()) openPrompt(LLMUIState::HOME);
            else sMenuStatus = "No model loaded";
            break;
          case LlmHomeAction::Guided:
            if (llmMenuGroupCount() > 0) enterPickGroup();
            break;
          case LlmHomeAction::NewChat:
            clearChatForOled();
            break;
          case LlmHomeAction::Rerun:
            rerunLast();
            openChatView();
            break;
          case LlmHomeAction::Model:
            enterPickModel();
            break;
          case LlmHomeAction::Back:
          case LlmHomeAction::Terminal:   // glasses only; never listed here
            break;
        }
        return true;
      }
      return false;   // B falls through to the global back-handler: leave the app
    }

    case LLMUIState::PICK_MODEL: {
      if (oledScrollHandleNav(&sModelScroll)) { sMenuStatus = nullptr; return true; }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
        clearChatForOled();
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        commitModelPick();
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        sMenuStatus = nullptr;
        enterHome();   // as the glasses' picker returns to its submenu
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
        enterHome();   // back one level
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
  sMenuStatus = nullptr;
  sLoadReturnsHome = false;
  // The app opens on its home menu, as on the glasses, with the cursor on top.
  ensurePickScrollInit();
  sHomeScroll.selectedIndex = 0;
  sHomeScroll.scrollOffset  = 0;
  sUIState = LLMUIState::HOME;
  sReplyStale = true;
  sReaderTop = 0;
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
