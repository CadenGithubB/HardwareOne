// OLED_Mode_Automations.cpp - Automation list and management OLED mode
// Full automation browser: list, select, run, enable/disable

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_AUTOMATION

#include <Adafruit_SSD1306.h>
#include "HAL_Input.h"
#include "System_Automation.h"
#include "System_Settings.h"
#include "System_EventCatalog.h"  // shared indexed event picker
#include "OLED_Utils.h"   // oledRunCommand, executeOLEDCommandWithResult

// External references
extern bool oledMenuBack();
extern void broadcastOutput(const String& msg);

// ============================================================================
// Automation List Data Model
// ============================================================================

#define AUTO_LIST_MAX 20
#define AUTO_NAME_MAX 20

// Rows are the automation owner's summary copies (System_Automation.h): id,
// name, enabled, command count and the trigger text the scheduler runs by.
struct AutomationRenderData {
  AutomationSummary items[AUTO_LIST_MAX];
  int count;
  int selectedIdx;
  bool valid;
  unsigned long lastRefresh;
};

EXT_RAM_BSS_ATTR static AutomationRenderData autoRenderData;
static const unsigned long AUTO_REFRESH_INTERVAL = 5000;

// Action feedback
static const char* autoActionMsg = nullptr;
static unsigned long autoActionMsgTime = 0;
static bool autoForceRefresh = false;

// How long an action toast (Ran/Done/Armed/Disabled/Deleted/...) stays up.
static const unsigned long AUTO_TOAST_MS = 1500;

// Show a brief action toast in the list footer. The OLED render gate is
// dirty-driven (oledIsDirty), so a toast set right after a single input frame
// would paint exactly once and then freeze — it would never update to a result
// and never auto-dismiss. Marking the display dirty across the toast window
// makes it actually animate in and clear itself once the window elapses.
static void autoShowToast(const char* msg) {
  autoActionMsg = msg;
  autoActionMsgTime = millis();
  oledMarkDirtyUntil(autoActionMsgTime + AUTO_TOAST_MS + 100);  // +margin so the clear frame renders
}

// Pending delete target: Y arms the global confirm overlay, whose callback
// (autoDeleteConfirmed) reads this id after the user says yes. The name is
// snapshotted into its own buffer because oledConfirmRequest stores the line
// pointer without copying, and the 5s list refresh can rewrite items[] while
// the dialog is still open.
static long sPendingDeleteId = 0;
static char sPendingDeleteName[AUTO_NAME_MAX] = {0};

// ============================================================================
// Guided "New automation" wizard
// ============================================================================
// Authoring on-device (the OLED can do more than view/run/toggle now). Emits a
// real `automationadd ...` command through executeOLEDCommandWithResult — the
// same canonical creation path the web UI uses via /api/cli — never a direct
// JSON write (settings-save-via-real-commands rule). Deliberately scoped: one
// trigger, no condition expressions (a gamepad is the wrong tool for
// `battery<20 AND wifi=up` — those stay on the web), no multi-trigger, no
// edit-in-place. The G2 stays view/run only (more limited interface).
//
// Flow: NAME (keyboard) -> TYPE (pick-list) -> schedule step (per type) ->
// COMMAND (keyboard) -> CONFIRM. B aborts the whole wizard from any step; the
// list refreshes on a successful add.
enum AutoWizStep : uint8_t {
  AW_NONE = 0,
  AW_NAME,        // keyboard
  AW_TYPE,        // pick-list: Interval / Daily / Event / Delay
  AW_INTERVAL,    // pick-list: interval presets
  AW_DELAY,       // pick-list: delay presets
  AW_DAILY,       // keyboard (numeric): HH:MM
  AW_EVENT_FAM,   // pick-list: event family
  AW_EVENT_KIND,  // pick-list: event kind within family
  AW_COMMAND,     // keyboard
  AW_CONFIRM      // summary + A=create
};
static AutoWizStep sWizStep = AW_NONE;
static bool sWizKbActive = false;

// Name and command hold everything the wizard keyboard can return
// (OLED_KEYBOARD_MAX_LENGTH characters + NUL). PSRAM: main-loop UI state only.
EXT_RAM_BSS_ATTR static char sWizName[OLED_KEYBOARD_MAX_LENGTH + 1];
EXT_RAM_BSS_ATTR static char sWizCommand[OLED_KEYBOARD_MAX_LENGTH + 1];
static char sWizTime[8]     = {0};   // "HH:MM"
static int  sWizTypeIdx     = 0;     // index into kWizTypes
static unsigned long sWizIntervalMs = 0;
static unsigned long sWizDelayMs    = 0;
static int  sWizCursor = 0;          // pick-list cursor
static int  sWizScroll = 0;          // pick-list scroll offset
static int  sWizConfirmScroll = 0;   // first confirm-screen row shown
static char sWizResultMsg[24] = {0}; // failure toast (autoActionMsg points here)

// Type options -> (menu label, wire type= value).
struct WizType { const char* label; const char* wireType; };
static const WizType kWizTypes[] = {
  { "Interval",   "interval"   },
  { "Daily @time","atTime"     },
  { "On event",   "event"      },
  { "Delay",      "afterDelay" },
};
static const int kWizTypeCount = (int)(sizeof(kWizTypes) / sizeof(kWizTypes[0]));

// Preset ladders (the web uses free numeric + unit; the OLED offers presets
// since typing ms on a gamepad keyboard is miserable).
struct WizPreset { const char* label; unsigned long ms; };
static const WizPreset kWizIntervals[] = {
  { "5 min",  300000UL   }, { "15 min", 900000UL   }, { "30 min", 1800000UL },
  { "1 hour", 3600000UL  }, { "6 hours",21600000UL }, { "24 hours",86400000UL },
};
static const int kWizIntervalCount = (int)(sizeof(kWizIntervals) / sizeof(kWizIntervals[0]));
static const WizPreset kWizDelays[] = {
  { "10 sec", 10000UL }, { "30 sec", 30000UL }, { "1 min", 60000UL },
  { "5 min",  300000UL }, { "15 min", 900000UL },
};
static const int kWizDelayCount = (int)(sizeof(kWizDelays) / sizeof(kWizDelays[0]));

// The selected family is an immutable provider ordinal, not a copied list.
// Every visible kind resolves directly through the shared family index, so a
// future family can grow beyond the former 24-pointer cache without truncation.
static size_t sWizEventFamilyIndex = SIZE_MAX;
static size_t sWizEventKindIndex = SIZE_MAX;

static bool wizFamilyAt(int index, SystemEventCatalogFamilyInfo* out) {
  return index >= 0 &&
         systemEventCatalogFamilyAt(static_cast<size_t>(index), out);
}

static bool wizSelectedFamily(SystemEventCatalogFamilyInfo* out) {
  return systemEventCatalogFamilyAt(sWizEventFamilyIndex, out);
}

static bool wizKindAt(int index, SystemEventCatalogKindInfo* out) {
  if (index < 0 || !out) return false;
  SystemEventCatalogFamilyInfo family{};
  return wizSelectedFamily(&family) &&
         systemEventCatalogFamilyKindAt(
             family.id, static_cast<size_t>(index), out);
}

static bool wizSelectedKind(SystemEventCatalogKindInfo* out) {
  if (!out) return false;
  SystemEventCatalogFamilyInfo family{};
  return wizSelectedFamily(&family) &&
         systemEventCatalogFamilyKindAt(
             family.id, sWizEventKindIndex, out);
}

static int wizListCount() {
  switch (sWizStep) {
    case AW_TYPE:       return kWizTypeCount;
    case AW_INTERVAL:   return kWizIntervalCount;
    case AW_DELAY:      return kWizDelayCount;
    case AW_EVENT_FAM:
      return static_cast<int>(systemEventCatalogFamilyCount());
    case AW_EVENT_KIND: {
      SystemEventCatalogFamilyInfo family{};
      return wizSelectedFamily(&family)
                 ? static_cast<int>(family.kindCount)
                 : 0;
    }
    default:            return 0;
  }
}

static const char* wizListLabel(int idx) {
  switch (sWizStep) {
    case AW_TYPE:       return kWizTypes[idx].label;
    case AW_INTERVAL:   return kWizIntervals[idx].label;
    case AW_DELAY:      return kWizDelays[idx].label;
    case AW_EVENT_FAM: {
      SystemEventCatalogFamilyInfo family{};
      return wizFamilyAt(idx, &family) ? family.label : "";
    }
    case AW_EVENT_KIND: {
      SystemEventCatalogKindInfo kind{};
      return wizKindAt(idx, &kind) ? kind.name : "";
    }
    default:            return "";
  }
}

static const char* wizListTitle() {
  switch (sWizStep) {
    case AW_TYPE:       return "Trigger:";
    case AW_INTERVAL:   return "Every:";
    case AW_DELAY:      return "After delay:";
    case AW_EVENT_FAM:  return "Event group:";
    case AW_EVENT_KIND: return "Event:";
    default:            return "";
  }
}

static bool wizStepIsList() {
  return sWizStep == AW_TYPE || sWizStep == AW_INTERVAL || sWizStep == AW_DELAY ||
         sWizStep == AW_EVENT_FAM || sWizStep == AW_EVENT_KIND;
}

static void wizAbort() {
  if (sWizKbActive) { oledKeyboardReset(); sWizKbActive = false; }
  sWizStep = AW_NONE;
}

static void wizResetAll() {
  wizAbort();
  sWizName[0] = sWizCommand[0] = sWizTime[0] = '\0';
  sWizTypeIdx = 0; sWizIntervalMs = sWizDelayMs = 0;
  sWizEventFamilyIndex = SIZE_MAX;
  sWizEventKindIndex = SIZE_MAX;
  sWizCursor = sWizScroll = sWizConfirmScroll = 0;
}

void oledAutomationsModeResetSessionState() {
  wizResetAll();
  memset(sWizResultMsg, 0, sizeof(sWizResultMsg));
}

// Open a keyboard step. `numbers` preselects the digits/symbols mode (the mode
// is not locked — the user can still cycle it — so numeric fields are
// re-validated on completion).
static void wizStartKeyboard(AutoWizStep step, const char* title, bool numbers) {
  sWizStep = step;
  // Automation names/times are plain labels. The command step is an opaque
  // executable payload and may itself contain credentials, so it never admits
  // microphone text.
  const OLEDKeyboardDictationPolicy policy =
      step == AW_COMMAND ? OLEDKeyboardDictationPolicy::DENY
                         : OLEDKeyboardDictationPolicy::ALLOW_PLAINTEXT;
  oledKeyboardInit(title, "", OLED_KEYBOARD_MAX_LENGTH, policy);
  if (numbers) gOledKeyboardState.mode = KEYBOARD_MODE_NUMBERS;
  sWizKbActive = true;
}

static void wizEnterList(AutoWizStep step) {
  sWizStep = step;
  sWizCursor = 0;
  sWizScroll = 0;
}

// Escape a value for a quoted key="..." token. CommandArgs::value() unescapes
// only \" (other backslashes pass through), so we escape ONLY the quote.
static String wizQuote(const char* s) {
  String out = "\"";
  for (const char* p = s; *p; p++) {
    if (*p == '"') out += '\\';
    out += *p;
  }
  out += '"';
  return out;
}

// Build and dispatch the automationadd command. enabled=1 (the parser defaults
// to DISABLED). Result "OK" = success; "ERROR" = failure (detail broadcast to
// the log/CLI viewer, not returned here).
static void wizCreate() {
  const char* wt = kWizTypes[sWizTypeIdx].wireType;
  String cmd = "automationadd name=";
  cmd += wizQuote(sWizName);
  cmd += " type=";
  cmd += wt;
  if (strcmp(wt, "interval") == 0)          cmd += " intervalms=" + String(sWizIntervalMs);
  else if (strcmp(wt, "atTime") == 0)       cmd += " time=" + String(sWizTime);
  else if (strcmp(wt, "event") == 0) {
    SystemEventCatalogKindInfo kind{};
    if (!wizSelectedKind(&kind)) {
      snprintf(sWizResultMsg, sizeof(sWizResultMsg), "Event unavailable");
      autoShowToast(sWizResultMsg);
      return;
    }
    cmd += " on=" + String(kind.name);
  }
  else if (strcmp(wt, "afterDelay") == 0)   cmd += " delayms=" + String(sWizDelayMs);
  cmd += " command=";
  cmd += wizQuote(sWizCommand);
  cmd += " enabled=1";

  char out[64];
  bool ok = executeOLEDCommandWithResult(cmd, out, sizeof(out));
  const bool failed = !ok || strncmp(out, "ERROR", 5) == 0 || strncmp(out, "Error", 5) == 0;
  if (failed) {
    // Detail is in the log; keep the user on CONFIRM so A retries, B cancels.
    snprintf(sWizResultMsg, sizeof(sWizResultMsg), "Add failed");
    autoShowToast(sWizResultMsg);
    return;
  }
  autoShowToast("Added");
  autoForceRefresh = true;
  sWizStep = AW_NONE;
}

// A-select on a pick-list step: commit the choice and advance.
static void wizListSelect() {
  switch (sWizStep) {
    case AW_TYPE:
      sWizTypeIdx = sWizCursor;
      switch (sWizCursor) {
        case 0: wizEnterList(AW_INTERVAL); break;
        case 1: wizStartKeyboard(AW_DAILY, "Time HH:MM", true); break;
        case 2: wizEnterList(AW_EVENT_FAM); break;
        case 3: wizEnterList(AW_DELAY); break;
      }
      break;
    case AW_INTERVAL:
      sWizIntervalMs = kWizIntervals[sWizCursor].ms;
      wizStartKeyboard(AW_COMMAND, "Command:", false);
      break;
    case AW_DELAY:
      sWizDelayMs = kWizDelays[sWizCursor].ms;
      wizStartKeyboard(AW_COMMAND, "Command:", false);
      break;
    case AW_EVENT_FAM:
      {
        SystemEventCatalogFamilyInfo family{};
        if (wizFamilyAt(sWizCursor, &family) && family.kindCount > 0) {
          sWizEventFamilyIndex = static_cast<size_t>(sWizCursor);
          sWizEventKindIndex = SIZE_MAX;
          wizEnterList(AW_EVENT_KIND);
        }
      }
      break;
    case AW_EVENT_KIND: {
      SystemEventCatalogKindInfo kind{};
      if (wizKindAt(sWizCursor, &kind)) {
        sWizEventKindIndex = static_cast<size_t>(sWizCursor);
        wizStartKeyboard(AW_COMMAND, "Command:", false);
      }
      break;
    }
    default: break;
  }
}

// Copies keyboard text whole into a wizard field. The fields hold the
// keyboard's full capacity; text that does not fit is refused (false) rather
// than stored cut short.
static bool wizStoreText(char* dst, size_t dstSize, const String& text) {
  if (text.length() >= dstSize) return false;
  memcpy(dst, text.c_str(), text.length() + 1);
  return true;
}

// Keyboard completion for the current step. Returns to advance; on invalid
// input re-opens the same keyboard with a toast.
static void wizKeyboardDone(const String& textIn) {
  String text = textIn;
  text.trim();
  switch (sWizStep) {
    case AW_NAME:
      if (text.length() == 0) {
        wizStartKeyboard(AW_NAME, "Name:", false);
        return;
      }
      if (!wizStoreText(sWizName, sizeof(sWizName), text)) {
        autoShowToast("Too long");
        wizStartKeyboard(AW_NAME, "Name:", false);
        return;
      }
      wizEnterList(AW_TYPE);
      return;
    case AW_DAILY: {
      // Strict HH:MM, same shape the parser demands.
      bool good = text.length() == 5 && text[2] == ':' &&
                  isdigit(text[0]) && isdigit(text[1]) &&
                  isdigit(text[3]) && isdigit(text[4]) &&
                  text.substring(0, 2).toInt() < 24 && text.substring(3).toInt() < 60;
      if (!good) {
        autoShowToast("Need HH:MM");
        wizStartKeyboard(AW_DAILY, "Time HH:MM", true);
        return;
      }
      strncpy(sWizTime, text.c_str(), sizeof(sWizTime) - 1);
      sWizTime[sizeof(sWizTime) - 1] = '\0';
      wizStartKeyboard(AW_COMMAND, "Command:", false);
      return;
    }
    case AW_COMMAND:
      if (text.length() == 0) {
        wizStartKeyboard(AW_COMMAND, "Command:", false);
        return;
      }
      if (!wizStoreText(sWizCommand, sizeof(sWizCommand), text)) {
        autoShowToast("Too long");
        wizStartKeyboard(AW_COMMAND, "Command:", false);
        return;
      }
      sWizConfirmScroll = 0;
      sWizStep = AW_CONFIRM;
      return;
    default:
      return;
  }
}

// Render a generic pick-list step (title + scroll window). Shared by all
// AW_*-list steps via wizListCount/wizListLabel.
static void wizDrawList() {
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
  oledDisplay->print(wizListTitle());

  const int n = wizListCount();
  const int lineH = 10;
  const int listTop = OLED_CONTENT_START_Y + lineH;
  const int vis = ((OLED_CONTENT_HEIGHT - lineH) / lineH) > 0 ? (OLED_CONTENT_HEIGHT - lineH) / lineH : 1;
  if (sWizCursor < sWizScroll) sWizScroll = sWizCursor;
  else if (sWizCursor >= sWizScroll + vis) sWizScroll = sWizCursor - vis + 1;

  int y = listTop;
  for (int i = 0; i < vis && (sWizScroll + i) < n; i++) {
    const int idx = sWizScroll + i;
    if (idx == sWizCursor) {
      oledDisplay->fillRect(0, y, 128, lineH, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }
    oledDisplay->setCursor(2, y + 1);
    char row[22];
    snprintf(row, sizeof(row), "%s", wizListLabel(idx));
    oledDisplay->print(row);
    y += lineH;
  }
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  if (sWizScroll > 0) { oledDisplay->setCursor(122, listTop); oledDisplay->print("\x18"); }
  if (sWizScroll + vis < n) { oledDisplay->setCursor(122, listTop + (vis - 1) * lineH); oledDisplay->print("\x19"); }
}

// Confirm screen rows: the name, the trigger, then the whole command in
// 18-column pieces ("> " on the first, "  " on the rest), so every character
// the automation will run is on screen. The rows that fit are drawn with the
// "A:Create B:Cancel" line under them; Up/Down scroll the rows when the
// command needs more, and column 21 holds the scroll arrows.
static const int kWizConfirmLineH = 9;
static const size_t kWizConfirmCmdCols = 18;

static int wizConfirmRowCount() {
  const size_t len = strlen(sWizCommand);
  const size_t cmdRows = len == 0 ? 1 : (len + kWizConfirmCmdCols - 1) / kWizConfirmCmdCols;
  return 2 + (int)cmdRows;
}

// Rows above the action line.
static int wizConfirmVisibleRows() {
  const int rows = OLED_CONTENT_HEIGHT / kWizConfirmLineH - 1;
  return rows > 0 ? rows : 1;
}

static int wizConfirmMaxScroll() {
  const int over = wizConfirmRowCount() - wizConfirmVisibleRows();
  return over > 0 ? over : 0;
}

static void wizConfirmRowText(int row, char* line, size_t size) {
  if (row == 0) {
    snprintf(line, size, "New: %.14s", sWizName);
    return;
  }
  if (row == 1) {
    const char* wt = kWizTypes[sWizTypeIdx].wireType;
    if (strcmp(wt, "interval") == 0)        snprintf(line, size, "Every %lum", (unsigned long)(sWizIntervalMs / 60000UL));
    else if (strcmp(wt, "atTime") == 0)     snprintf(line, size, "Daily %s", sWizTime);
    else if (strcmp(wt, "event") == 0) {
      SystemEventCatalogKindInfo kind{};
      snprintf(line, size, "On %.16s",
               wizSelectedKind(&kind) ? kind.name : "unavailable");
    }
    else                                    snprintf(line, size, "Delay %lus", (unsigned long)(sWizDelayMs / 1000UL));
    return;
  }
  const size_t offset = (size_t)(row - 2) * kWizConfirmCmdCols;
  const size_t len = strlen(sWizCommand);
  snprintf(line, size, "%s%.*s", row == 2 ? "> " : "  ", (int)kWizConfirmCmdCols,
           sWizCommand + (offset < len ? offset : len));
}

static void wizDrawConfirm() {
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  const int rows = wizConfirmRowCount();
  const int vis = wizConfirmVisibleRows();
  const int maxScroll = wizConfirmMaxScroll();
  if (sWizConfirmScroll > maxScroll) sWizConfirmScroll = maxScroll;
  if (sWizConfirmScroll < 0) sWizConfirmScroll = 0;

  int y = OLED_CONTENT_START_Y;
  char line[24];
  for (int i = 0; i < vis && sWizConfirmScroll + i < rows; i++) {
    wizConfirmRowText(sWizConfirmScroll + i, line, sizeof(line));
    oledDisplay->setCursor(0, y); oledDisplay->print(line);
    y += kWizConfirmLineH;
  }
  if (sWizConfirmScroll > 0) {
    oledDisplay->setCursor(122, OLED_CONTENT_START_Y); oledDisplay->print("\x18");
  }
  if (sWizConfirmScroll < maxScroll) {
    oledDisplay->setCursor(122, OLED_CONTENT_START_Y + (vis - 1) * kWizConfirmLineH);
    oledDisplay->print("\x19");
  }

  oledDisplay->setCursor(0, y); oledDisplay->print("A:Create B:Cancel");
}

// Wizard render entry — called from displayAutomations when sWizStep != AW_NONE
// (the keyboard curtain is handled by the caller first).
static void wizDraw() {
  if (sWizStep == AW_CONFIRM) { wizDrawConfirm(); return; }
  if (wizStepIsList())        { wizDrawList();    return; }
  // Keyboard steps draw nothing here — the curtain owns the screen while the
  // keyboard is active; between events there is nothing to show.
}

// Wizard input entry — called from automationsInputHandler when the wizard is
// active and the keyboard is NOT (keyboard input is centrally intercepted).
// Returns true (input consumed).
static bool wizHandleInput(uint32_t newlyPressed) {
  extern NavEvents gNavEvents;
  if (sWizStep == AW_CONFIRM) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) { wizCreate(); return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) { wizAbort();  return true; }
    if (gNavEvents.up && sWizConfirmScroll > 0) sWizConfirmScroll--;
    else if (gNavEvents.down && sWizConfirmScroll < wizConfirmMaxScroll()) sWizConfirmScroll++;
    return true;
  }
  if (wizStepIsList()) {
    const int n = wizListCount();
    if (n > 0 && gNavEvents.up)   { sWizCursor = (sWizCursor + n - 1) % n; return true; }
    if (n > 0 && gNavEvents.down) { sWizCursor = (sWizCursor + 1) % n;     return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) { if (n > 0) wizListSelect(); return true; }
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) { wizAbort(); return true; }
    return true;
  }
  // Keyboard step but keyboard not active (shouldn't happen) — bail out.
  return true;
}

// ============================================================================
// Prepare Automation Data (called OUTSIDE I2C transaction)
// ============================================================================

void prepareAutomationData() {
  if (!gSettings.automationEnabled) {
    autoRenderData.valid = false;
    autoRenderData.count = 0;
    return;
  }

  unsigned long now = millis();
  if (!autoForceRefresh && autoRenderData.valid && autoRenderData.lastRefresh > 0 &&
      (now - autoRenderData.lastRefresh) < AUTO_REFRESH_INTERVAL) {
    return;
  }
  autoForceRefresh = false;

  size_t count = 0;
  const bool ok = automationCopySummaries(autoRenderData.items, AUTO_LIST_MAX, &count);

  autoRenderData.count = (int)count;
  autoRenderData.valid = ok;
  autoRenderData.lastRefresh = now;

  // Selection may land on the virtual "+ New" row at index == count (so the
  // clamp allows count, not count-1). count==0 → only the "+ New" row exists.
  if (autoRenderData.selectedIdx > autoRenderData.count) {
    autoRenderData.selectedIdx = autoRenderData.count;
  }
  if (autoRenderData.selectedIdx < 0) autoRenderData.selectedIdx = 0;
}

// ============================================================================
// Display Automations (called INSIDE I2C transaction)
// ============================================================================

// Copies the next line of `text` (at most `width` chars) into `line` and
// returns the rest. Breaks at the last space that fits, or mid-word when one
// word is wider than the line.
static const char* autoWrapLine(const char* text, char* line, size_t width) {
  while (*text == ' ') text++;
  size_t take = strlen(text);
  if (take > width) {
    size_t space = width;
    while (space > 0 && text[space] != ' ') space--;
    take = (space > 0) ? space : width;
  }
  memcpy(line, text, take);
  line[take] = '\0';
  return text + take;
}

void displayAutomations() {
  if (!oledDisplay || !oledConnected) return;

  // Create-wizard: the keyboard curtain owns the screen while a keyboard step
  // is active; otherwise the wizard renders its own list/confirm.
  if (oledKeyboardDrawIfActive(oledDisplay)) return;
  if (sWizStep != AW_NONE) { wizDraw(); return; }

  if (!gSettings.automationEnabled) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 8);
    oledDisplay->println("Automations disabled");
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 22);
    oledDisplay->println("Press X to enable");
    return;
  }

  if (!autoRenderData.valid) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("Loading...");
    return;
  }

  const int listWidth = 78;
  const int detailX = 86;
  const int itemHeight = 10;
  const int maxVisibleItems = 4;
  const int startY = OLED_CONTENT_START_Y + 1;

  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  // Vertical separator
  oledDisplay->drawFastVLine(84, OLED_CONTENT_START_Y, OLED_CONTENT_HEIGHT, DISPLAY_COLOR_WHITE);

  // Total navigable rows = the automations plus one virtual "+ New" row at
  // index == count.
  const int totalRows = autoRenderData.count + 1;

  // Scroll offset
  int scrollOffset = 0;
  if (autoRenderData.selectedIdx >= maxVisibleItems) {
    scrollOffset = autoRenderData.selectedIdx - maxVisibleItems + 1;
  }

  // === Left Panel: Automation List (+ trailing "+ New" row) ===
  for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < totalRows; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * itemHeight;
    bool isSelected = (idx == autoRenderData.selectedIdx);
    const bool isNewRow = (idx == autoRenderData.count);

    if (isSelected) {
      oledDisplay->fillRect(0, y, listWidth, itemHeight - 1, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }

    if (isNewRow) {
      oledDisplay->setCursor(2, y + 1);
      oledDisplay->print("+ New");
      continue;
    }

    const AutomationSummary& item = autoRenderData.items[idx];
    // Status dot: filled = enabled, hollow = disabled
    int dotX = 2;
    int dotY = y + 3;
    uint16_t dotColor = isSelected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
    if (item.enabled) {
      oledDisplay->fillCircle(dotX + 1, dotY + 1, 2, dotColor);
    } else {
      oledDisplay->drawCircle(dotX + 1, dotY + 1, 2, dotColor);
    }

    // Name (truncated to fit)
    oledDisplay->setCursor(8, y + 1);
    char truncName[13];
    strncpy(truncName, item.name, 12);
    truncName[12] = '\0';
    oledDisplay->print(truncName);
  }

  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  // === Right Panel: Selected Item Details (or the "+ New" hint) ===
  if (autoRenderData.selectedIdx == autoRenderData.count) {
    oledDisplay->setCursor(detailX, OLED_CONTENT_START_Y + 2);
    oledDisplay->print("Create");
    oledDisplay->setCursor(detailX, OLED_CONTENT_START_Y + 14);
    oledDisplay->print("A: New");
  } else if (autoRenderData.selectedIdx < autoRenderData.count) {
    const AutomationSummary& sel = autoRenderData.items[autoRenderData.selectedIdx];
    int dy = OLED_CONTENT_START_Y + 2;

    // Trigger text ("daily 07:30", "every 5m", "on peer_online", ...) over at
    // most three 7-column lines (the pane is ~42 px; GFX auto-wrap would spill
    // into the list).
    const char* rest = sel.trigger;
    for (int line = 0; line < 3 && *rest; line++) {
      char seg[8];
      rest = autoWrapLine(rest, seg, sizeof(seg) - 1);
      oledDisplay->setCursor(detailX, dy);
      oledDisplay->print(seg);
      dy += 10;
    }

    // Enabled status
    oledDisplay->setCursor(detailX, dy);
    oledDisplay->print(sel.enabled ? "ON" : "OFF");
    dy += 10;

    // Command count
    oledDisplay->setCursor(detailX, dy);
    char cmdBuf[12];
    snprintf(cmdBuf, sizeof(cmdBuf), "%d cmd%s", sel.commandCount, sel.commandCount != 1 ? "s" : "");
    oledDisplay->print(cmdBuf);
  }

  // Scroll indicators
  if (scrollOffset > 0) {
    oledDisplay->setCursor(78, OLED_CONTENT_START_Y);
    oledDisplay->print("^");
  }
  if (scrollOffset + maxVisibleItems < totalRows) {
    oledDisplay->setCursor(78, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9);
    oledDisplay->print("v");
  }

  // Page indicator in header area
  char pageStr[8];
  snprintf(pageStr, sizeof(pageStr), "%d/%d", autoRenderData.selectedIdx + 1, totalRows);
  int pageStrWidth = strlen(pageStr) * 6;
  oledDisplay->setCursor(128 - pageStrWidth, 0);
  oledDisplay->print(pageStr);

  // Action feedback overlay
  if (autoActionMsg && (millis() - autoActionMsgTime) < AUTO_TOAST_MS) {
    int msgY = OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9;
    oledDisplay->fillRect(0, msgY, 84, 9, DISPLAY_COLOR_WHITE);
    oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    oledDisplay->setCursor(2, msgY + 1);
    oledDisplay->print(autoActionMsg);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  } else {
    autoActionMsg = nullptr;
  }
}

// ============================================================================
// Navigation and Actions
// ============================================================================

// One row per nav event; the input layer (gNavEvents) paces joystick repeats.
static void autoListUp() {
  if (autoRenderData.count == 0) return;
  if (autoRenderData.selectedIdx > 0)
    autoRenderData.selectedIdx--;
}

static void autoListDown() {
  // Reaches the virtual "+ New" row at index == count (so the last real row
  // isn't the end of travel — you can always scroll down to create).
  if (autoRenderData.selectedIdx < autoRenderData.count)
    autoRenderData.selectedIdx++;
}

static void autoRunSelected() {
  // The "+ New" virtual row (idx == count) is handled by the caller; here only
  // real automations run.
  if (autoRenderData.count == 0 || autoRenderData.selectedIdx >= autoRenderData.count) return;
  const AutomationSummary& sel = autoRenderData.items[autoRenderData.selectedIdx];

  // Manual (afterDelay) automations are manually-armed one-shots: "trigger"
  // arms the delay timer; "run" would execute immediately and bypass the delay.
  const bool isAfterDelay = (sel.kind == AutomationTriggerKind::Manual);

  char cmd[48];
  if (isAfterDelay) {
    snprintf(cmd, sizeof(cmd), "automation trigger id=%ld", sel.id);
  } else {
    snprintf(cmd, sizeof(cmd), "automationrun id=%ld", sel.id);
  }

  // The dispatch is synchronous: by the time it returns we know whether the
  // trigger succeeded (the automation's commands then run async on the exec
  // queue). Show a terminal result — the old "Running..." was set AFTER the
  // blocking call already returned, so it never reflected success/failure and,
  // without a dirty-window, froze on screen forever.
  char out[64];
  bool ok = executeOLEDCommandWithResult(String(cmd), out, sizeof(out));
  bool failed = !ok || strncmp(out, "ERROR", 5) == 0 || strncmp(out, "Error", 5) == 0;

  if (isAfterDelay) {
    autoShowToast(failed ? "Arm failed" : "Armed");
  } else {
    autoShowToast(failed ? "Run failed" : "Done");
  }
}

static void autoToggleSelected() {
  if (autoRenderData.count == 0 || autoRenderData.selectedIdx >= autoRenderData.count) return;
  const AutomationSummary& sel = autoRenderData.items[autoRenderData.selectedIdx];

  char cmd[48];
  snprintf(cmd, sizeof(cmd), "automation %s id=%ld",
           sel.enabled ? "disable" : "enable", sel.id);
  // A refusal (admin-only command, not the creator) is toasted by the helper.
  if (oledRunCommand(String(cmd))) {
    autoShowToast(sel.enabled ? "Disabled" : "Enabled");
  }

  // Force data refresh on next frame
  autoForceRefresh = true;
}

// Confirm-overlay callback: delete the pending automation through the shared
// core verb (the same path web/CLI use), then force a list refresh. The list's
// selectedIdx re-clamps in prepareAutomationData() once the row is gone.
static void autoDeleteConfirmed(void* /*userData*/) {
  char cmd[48];
  snprintf(cmd, sizeof(cmd), "automation delete id=%ld", sPendingDeleteId);
  if (oledRunCommand(String(cmd))) autoShowToast("Deleted");
  autoForceRefresh = true;
}

static void autoBack() {
  autoRenderData.valid = false;
  autoRenderData.lastRefresh = 0;
  oledMenuBack();
}

// ============================================================================
// Input Handler
// ============================================================================

static bool automationsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  extern NavEvents gNavEvents;

  // Create-wizard keyboard completion poll (CLIInput idiom). While a keyboard
  // step is active this handler is never called — the central pump owns input
  // — so completion/cancel is observed on the NEXT event after it clears.
  if (sWizKbActive) {
    if (oledKeyboardIsCompleted()) {
      String text = oledKeyboardGetText();
      oledKeyboardReset();
      sWizKbActive = false;
      wizKeyboardDone(text);
      return true;
    }
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      sWizKbActive = false;
      wizAbort();
      return true;
    }
    return false;  // keyboard still active — central pump owns input
  }

  // Wizard (non-keyboard steps: pick-lists, confirm) owns input while open.
  if (sWizStep != AW_NONE) {
    return wizHandleInput(newlyPressed);
  }

  if (gNavEvents.down) {
    autoListDown();
    return true;
  }
  if (gNavEvents.up) {
    autoListUp();
    return true;
  }

  // A = Run selected automation, or start the create wizard on the "+ New" row.
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (gSettings.automationEnabled && autoRenderData.selectedIdx == autoRenderData.count) {
      wizResetAll();
      wizStartKeyboard(AW_NAME, "Name:", false);
      return true;
    }
    autoRunSelected();
    return true;
  }
  // X = Enable system if disabled, or toggle selected automation
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (!gSettings.automationEnabled) {
      oledRunCommand("automation system enable");
      autoForceRefresh = true;
      return true;
    }
    autoToggleSelected();
    return true;
  }
  // Y = Delete selected automation (guarded by the global confirm overlay)
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_Y)) {
    if (gSettings.automationEnabled && autoRenderData.count > 0 &&
        autoRenderData.selectedIdx < autoRenderData.count) {
      const AutomationSummary& sel = autoRenderData.items[autoRenderData.selectedIdx];
      sPendingDeleteId = sel.id;
      strncpy(sPendingDeleteName, sel.name, sizeof(sPendingDeleteName) - 1);
      sPendingDeleteName[sizeof(sPendingDeleteName) - 1] = '\0';
      oledConfirmRequest("Delete automation?", sPendingDeleteName, autoDeleteConfirmed, nullptr);
    }
    return true;
  }
  // B = Back to menu
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    autoBack();
    return true;
  }
  return false;
}

// ============================================================================
// Mode Registration
// ============================================================================

// Fresh-visit reset so a Home-button escape mid-wizard can't leak a half-built
// automation (or a stuck keyboard) into the next entry.
static void automationsOnEnter(bool isForward) {
  if (isForward) wizResetAll();
}

// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints, onEnter
static const OLEDModeEntry sAutomationsModes[] = {
  { OLED_AUTOMATIONS, "Automations", "notify_automation", displayAutomations, nullptr, automationsInputHandler, false, -1, "A:Run X:On/Off Y:Del", automationsOnEnter },
};

REGISTER_OLED_MODE_MODULE(sAutomationsModes, sizeof(sAutomationsModes) / sizeof(sAutomationsModes[0]), "Automations");

#endif // ENABLE_OLED_DISPLAY && ENABLE_AUTOMATION
