// ============================================================================
// OLED LLM Chat Mode
// ============================================================================
// Chat-style interface: scrollable output above a single input line.
// A/X opens the keyboard to type a prompt; output streams in live.
// Up/down scrolls history; B stops generation or goes back.

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_ONDEVICE_LLM

#include <Adafruit_SSD1306.h>
#include "OLED_Utils.h"
#include "System_LLM.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ============================================================================
// Constants
// ============================================================================

static const int LLM_CHAT_CAP      = 32;   // Max stored lines (rolling)
static const int LLM_CHARS         = 21;   // Characters per line at text size 1
static const int LLM_LINE_H        = 10;   // Pixels per line
static const int LLM_VISIBLE_LINES = 3;    // Output lines shown above input bar
static const int LLM_INPUT_H       = 10;   // Input bar height
static const int LLM_GEN_STACK     = 10240; // FreeRTOS task stack for generation

// ============================================================================
// State
// ============================================================================

static char sLines[LLM_CHAT_CAP][LLM_CHARS + 1];
static int  sLineCount    = 0;
static int  sScrollOffset = 0;  // 0 = newest at bottom; positive = scrolled back

// Current token stream — appended live during generation, flushed on newline/wrap
static char sStreamLine[LLM_CHARS + 1] = "";
static int  sStreamLen = 0;

static volatile bool sGenerating  = false;
static volatile bool sStopRequest = false;

static bool sKeyboardActive = false;

static SemaphoreHandle_t sLLMMutex = nullptr;

// ============================================================================
// Buffer helpers  (must be called with sLLMMutex held)
// ============================================================================

static void _pushLine(const char* text) {
  if (sLineCount < LLM_CHAT_CAP) {
    strncpy(sLines[sLineCount], text, LLM_CHARS);
    sLines[sLineCount][LLM_CHARS] = '\0';
    sLineCount++;
  } else {
    memmove(sLines[0], sLines[1], (LLM_CHAT_CAP - 1) * sizeof(sLines[0]));
    strncpy(sLines[LLM_CHAT_CAP - 1], text, LLM_CHARS);
    sLines[LLM_CHAT_CAP - 1][LLM_CHARS] = '\0';
  }
  sScrollOffset = 0;  // Auto-scroll to bottom on new content
}

// Append a raw token string, hard-wrapping at LLM_CHARS
static void _appendToken(const char* token) {
  for (const char* p = token; *p; p++) {
    char c = *p;
    if (c == '\n' || sStreamLen >= LLM_CHARS) {
      _pushLine(sStreamLine);
      sStreamLine[0] = '\0';
      sStreamLen = 0;
      if (c == '\n') continue;
    }
    sStreamLine[sStreamLen++] = c;
    sStreamLine[sStreamLen]   = '\0';
  }
}

// ============================================================================
// Background generation task
// ============================================================================

struct LLMOLEDGenParams {
  char prompt[128];
};

static void llmOLEDGenTask(void* param) {
  auto* p = (LLMOLEDGenParams*)param;

  llmGenerate(p->prompt, [](const char* token) -> bool {
    if (sStopRequest) return false;
    if (sLLMMutex && xSemaphoreTake(sLLMMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      _appendToken(token);
      xSemaphoreGive(sLLMMutex);
    }
    return true;
  });

  // Flush any partial stream line
  if (sLLMMutex && xSemaphoreTake(sLLMMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (sStreamLen > 0) {
      _pushLine(sStreamLine);
      sStreamLine[0] = '\0';
      sStreamLen = 0;
    }
    xSemaphoreGive(sLLMMutex);
  }

  free(p);
  sGenerating = false;
  vTaskDelete(nullptr);
}

static void startLLMGen(const char* prompt) {
  if (sGenerating || !llmIsReady()) return;

  auto* p = (LLMOLEDGenParams*)malloc(sizeof(LLMOLEDGenParams));
  if (!p) return;
  strncpy(p->prompt, prompt, sizeof(p->prompt) - 1);
  p->prompt[sizeof(p->prompt) - 1] = '\0';

  sGenerating  = true;
  sStopRequest = false;

  if (xTaskCreate(llmOLEDGenTask, "llm_oled", LLM_GEN_STACK, p, 2, nullptr) != pdPASS) {
    free(p);
    sGenerating = false;
  }
}

// ============================================================================
// Display
// ============================================================================

static void displayLLM() {
  if (!oledDisplay) return;

  // --- Keyboard view ---
  if (sKeyboardActive && oledKeyboardIsActive()) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

    // One preview line at the top (most recent confirmed output)
    if (sLLMMutex && xSemaphoreTake(sLLMMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (sLineCount > 0) {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->print(sLines[sLineCount - 1]);
      }
      xSemaphoreGive(sLLMMutex);
    }
    int sepY = OLED_CONTENT_START_Y + LLM_LINE_H;
    oledDisplay->drawFastHLine(0, sepY, DISPLAY_WIDTH, DISPLAY_COLOR_WHITE);
    oledKeyboardDisplay(oledDisplay);
    return;
  }

  // --- Normal chat view ---
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  if (sLLMMutex && xSemaphoreTake(sLLMMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    int streamVisible = (sStreamLen > 0) ? 1 : 0;
    int totalLines    = sLineCount + streamVisible;

    // firstVisible: index of the oldest line shown in the output area
    int firstVisible = totalLines - LLM_VISIBLE_LINES - sScrollOffset;
    if (firstVisible < 0) firstVisible = 0;

    int y = OLED_CONTENT_START_Y;
    for (int i = 0; i < LLM_VISIBLE_LINES; i++) {
      int idx = firstVisible + i;
      if (idx >= totalLines) break;
      oledDisplay->setCursor(0, y);
      if (idx < sLineCount) {
        oledDisplay->print(sLines[idx]);
      } else {
        // Streaming line — show with blinking underscore
        oledDisplay->print(sStreamLine);
        if ((millis() / 500) % 2 == 0) oledDisplay->print("_");
      }
      y += LLM_LINE_H;
    }

    // Scroll-up indicator: there are older lines not shown above
    if (firstVisible > 0) {
      oledDisplay->setCursor(121, OLED_CONTENT_START_Y);
      oledDisplay->print("^");
    }
    // Scroll-down indicator: there are newer lines below (user scrolled back)
    if (sScrollOffset > 0) {
      oledDisplay->setCursor(121, OLED_CONTENT_START_Y + (LLM_VISIBLE_LINES - 1) * LLM_LINE_H);
      oledDisplay->print("v");
    }

    xSemaphoreGive(sLLMMutex);
  }

  // --- Separator ---
  int sepY = OLED_CONTENT_START_Y + LLM_VISIBLE_LINES * LLM_LINE_H;
  oledDisplay->drawFastHLine(0, sepY, DISPLAY_WIDTH, DISPLAY_COLOR_WHITE);

  // --- Input bar ---
  int inputY = sepY + 1;
  oledDisplay->setCursor(0, inputY);
  if (sGenerating) {
    static const char* spinFrames[] = {".  ", ".. ", "..."};
    oledDisplay->print(spinFrames[(millis() / 300) % 3]);
  } else {
    LLMStatus st = llmGetStatus();
    if (st.state == LLMState::UNLOADED || st.state == LLMState::ERROR) {
      oledDisplay->print("No model loaded");
    } else {
      oledDisplay->print("> ");
    }
  }
}

// ============================================================================
// Input
// ============================================================================

static bool handleLLMInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // --- Keyboard active: route completion/cancel, let keyboard consume navigation ---
  if (sKeyboardActive) {
    if (oledKeyboardIsCompleted()) {
      const char* text = oledKeyboardGetText();
      String prompt = String(text);
      oledKeyboardReset();
      sKeyboardActive = false;

      if (prompt.length() > 0) {
        // Echo the prompt into the chat buffer
        char echoBuf[LLM_CHARS + 1];
        snprintf(echoBuf, sizeof(echoBuf), ">%s", prompt.c_str());
        if (sLLMMutex && xSemaphoreTake(sLLMMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          _pushLine(echoBuf);
          xSemaphoreGive(sLLMMutex);
        }
        startLLMGen(prompt.c_str());
      }
      return true;
    }
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sKeyboardActive = false;
      return true;
    }
    return false;  // Keyboard handles its own navigation
  }

  // --- Generating: B stops, everything else ignored ---
  if (sGenerating) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      sStopRequest = true;
      return true;
    }
    return false;
  }

  // --- Idle: A/X opens keyboard, scroll with up/down, B goes back ---
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    LLMStatus st = llmGetStatus();
    if (st.state == LLMState::READY) {
      oledKeyboardInit("Prompt:", nullptr, OLED_KEYBOARD_MAX_LENGTH);
      sKeyboardActive = true;
    }
    return true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    return false;  // Let global handler call oledMenuBack()
  }

  // Scroll through history
  if (gNavEvents.up) {
    if (sLLMMutex && xSemaphoreTake(sLLMMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      int streamVisible = (sStreamLen > 0) ? 1 : 0;
      int totalLines    = sLineCount + streamVisible;
      int maxScroll     = totalLines > LLM_VISIBLE_LINES ? totalLines - LLM_VISIBLE_LINES : 0;
      if (sScrollOffset < maxScroll) sScrollOffset++;
      xSemaphoreGive(sLLMMutex);
    }
    return true;
  }
  if (gNavEvents.down) {
    if (sScrollOffset > 0) sScrollOffset--;
    return true;
  }

  return false;
}

// ============================================================================
// Availability
// ============================================================================

static bool isLLMAvailable(String* outReason) {
  LLMStatus st = llmGetStatus();
  if (st.state == LLMState::ERROR) {
    if (outReason) *outReason = "LLM error";
    return false;
  }
  return true;
}

// ============================================================================
// Mode reset (called on mode entry)
// ============================================================================

void resetLLMOLEDState() {
  if (!sLLMMutex) {
    sLLMMutex = xSemaphoreCreateMutex();
  }
  if (sKeyboardActive) {
    oledKeyboardReset();
    sKeyboardActive = false;
  }
  // Preserve chat history across re-entries
}

// ============================================================================
// Registration
// ============================================================================

static const OLEDModeEntry sLLMModeEntry = {
  OLED_LLM,
  "LLM Chat",
  "terminal",
  displayLLM,
  isLLMAvailable,
  handleLLMInput,
  true,   // show in main menu
  95,     // menu order (after CLI Input at 93)
  nullptr // hints vary by state; footer shows nothing extra
};

REGISTER_OLED_MODE_MODULE(&sLLMModeEntry, 1, "LLM");

// Force linker to include this file
void oledLLMModeInit() {}

#endif // ENABLE_OLED_DISPLAY && ENABLE_ONDEVICE_LLM
