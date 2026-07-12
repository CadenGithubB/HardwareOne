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
static WrappedLine sRenderLines[LLM_RENDER_LINES];
static int sRenderLineCount = 0;

// ============================================================================
// State transitions
// ============================================================================

// Refresh state from the engine + chat module. Called at the top of
// displayLLM() each frame so transitions land within one render tick.
static void syncStateFromEngine() {
  if (sKeyboardActive) return;  // keyboard owns the UI

  LLMStatus st = llmGetStatus();

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

  // Footer
  if (generating) {
    drawFooter("B: stop");
  } else if (chatGetTurnCount() > 0) {
    drawFooter("A: ask  Y: retry");
  } else {
    drawFooter("A: ask");
  }
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
    case LLMUIState::NO_MODEL:   displayLLM_noModel(); break;
    case LLMUIState::LOADING:    displayLLM_loading(); break;
    case LLMUIState::READY:      displayLLM_chat(false); break;
    case LLMUIState::GENERATING: displayLLM_chat(true);  break;
  }
}

// ============================================================================
// Input
// ============================================================================

static bool handleLLMInput(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  // Keyboard active: confirm/cancel are routed here; navigation handled internally.
  if (sKeyboardActive) {
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      String prompt = String(text);
      oledKeyboardReset();
      sKeyboardActive = false;
      if (prompt.length() > 0) {
        chatBeginTurn(prompt.c_str(), nullptr);
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
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        // Push the shared file picker scoped to /system/llm with *.bin filter.
        // The callback (onLLMModelPicked) handles the actual model load when
        // the file browser pops back here.
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
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
        oledKeyboardInit("Prompt:", nullptr, OLED_KEYBOARD_MAX_LENGTH);
        sKeyboardActive = true;
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
        // Retry — re-runs the last user prompt with the prior answer's tokens
        // as suppress. No effect if there's nothing to retry.
        chatRetryLast(nullptr);
        return true;
      }
      if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
        return false;  // global back
      }
      if (gNavEvents.up)   { sScrollOffset++; return true; }
      if (gNavEvents.down) { if (sScrollOffset > 0) sScrollOffset--; return true; }
      return false;
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
