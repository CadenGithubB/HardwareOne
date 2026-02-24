// OLED_Mode_Automations.cpp - Automation list and management OLED mode
// Full automation browser: list, select, run, enable/disable

#include "OLED_Display.h"
#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY && ENABLE_AUTOMATION

#include <Adafruit_SSD1306.h>
#include "HAL_Input.h"
#include "System_Automation.h"
#include "System_Settings.h"

// External references
extern bool oledConnected;
extern OLEDMode currentOLEDMode;
extern Settings gSettings;
extern const char* AUTOMATIONS_JSON_FILE;
extern bool oledMenuBack();
extern void executeOLEDCommand(const String& cmd);
extern void broadcastOutput(const String& msg);

// ============================================================================
// Automation List Data Model
// ============================================================================

#define AUTO_LIST_MAX 20
#define AUTO_NAME_MAX 20
#define AUTO_TYPE_MAX 12

struct AutoListItem {
  long id;
  char name[AUTO_NAME_MAX];
  char type[AUTO_TYPE_MAX];    // "atTime", "afterDelay", "interval"
  bool enabled;
  int commandCount;
  char timeStr[16];            // HH:MM or delay/interval string
};

struct AutomationRenderData {
  AutoListItem items[AUTO_LIST_MAX];
  int count;
  int selectedIdx;
  bool valid;
  unsigned long lastRefresh;
};

static AutomationRenderData autoRenderData = {};
static unsigned long autoLastInput = 0;
static const unsigned long AUTO_DEBOUNCE = 200;
static const unsigned long AUTO_REFRESH_INTERVAL = 5000;

// Action feedback
static const char* autoActionMsg = nullptr;
static unsigned long autoActionMsgTime = 0;
static bool autoForceRefresh = false;

// ============================================================================
// JSON Field Extraction Helpers (local, stack-only)
// ============================================================================

static bool extractStr(const char* json, const char* key, char* out, size_t outSize) {
  out[0] = '\0';
  const char* keyPos = strstr(json, key);
  if (!keyPos) return false;
  const char* colon = strchr(keyPos, ':');
  if (!colon) return false;
  const char* q1 = strchr(colon, '"');
  if (!q1) return false;
  q1++;
  const char* q2 = strchr(q1, '"');
  if (!q2) return false;
  size_t len = q2 - q1;
  if (len >= outSize) len = outSize - 1;
  strncpy(out, q1, len);
  out[len] = '\0';
  return true;
}

static long extractLong(const char* json, const char* key) {
  const char* keyPos = strstr(json, key);
  if (!keyPos) return 0;
  const char* colon = strchr(keyPos, ':');
  if (!colon) return 0;
  return atol(colon + 1);
}

static bool extractBool(const char* json, const char* key) {
  const char* keyPos = strstr(json, key);
  if (!keyPos) return false;
  const char* colon = strchr(keyPos, ':');
  if (!colon) return false;
  const char* p = colon + 1;
  while (*p == ' ' || *p == '\t') p++;
  return (strncmp(p, "true", 4) == 0);
}

// ============================================================================
// Data Gathering (streaming callback)
// ============================================================================

struct AutoGatherCtx {
  AutoListItem* items;
  int count;
  int maxCount;
};

static bool autoGatherCallback(const char* autoJson, size_t jsonLen, void* userData) {
  AutoGatherCtx* ctx = (AutoGatherCtx*)userData;
  if (ctx->count >= ctx->maxCount) return false;

  AutoListItem& item = ctx->items[ctx->count];
  memset(&item, 0, sizeof(AutoListItem));

  // Extract ID
  item.id = extractLong(autoJson, "\"id\"");
  if (item.id == 0) return true;

  // Extract name
  if (!extractStr(autoJson, "\"name\"", item.name, AUTO_NAME_MAX)) {
    snprintf(item.name, AUTO_NAME_MAX, "Auto #%ld", item.id);
  }

  // Extract type
  extractStr(autoJson, "\"type\"", item.type, AUTO_TYPE_MAX);

  // Extract enabled
  item.enabled = extractBool(autoJson, "\"enabled\"");

  // Extract time display string based on type
  if (strcmp(item.type, "atTime") == 0) {
    extractStr(autoJson, "\"time\"", item.timeStr, sizeof(item.timeStr));
  } else if (strcmp(item.type, "afterDelay") == 0 || strcmp(item.type, "afterdelay") == 0) {
    long ms = extractLong(autoJson, "\"delayMs\"");
    if (ms >= 60000)
      snprintf(item.timeStr, sizeof(item.timeStr), "%ldm", ms / 60000);
    else
      snprintf(item.timeStr, sizeof(item.timeStr), "%lds", ms / 1000);
  } else if (strcmp(item.type, "interval") == 0) {
    long ms = extractLong(autoJson, "\"intervalMs\"");
    if (ms >= 3600000)
      snprintf(item.timeStr, sizeof(item.timeStr), "q%ldh", ms / 3600000);
    else if (ms >= 60000)
      snprintf(item.timeStr, sizeof(item.timeStr), "q%ldm", ms / 60000);
    else
      snprintf(item.timeStr, sizeof(item.timeStr), "q%lds", ms / 1000);
  }

  // Count commands in array
  item.commandCount = 0;
  const char* cmdsKey = strstr(autoJson, "\"commands\"");
  if (cmdsKey) {
    const char* arrStart = strchr(cmdsKey, '[');
    if (arrStart) {
      int depth = 0;
      bool inStr = false;
      item.commandCount = 1;
      for (const char* p = arrStart; *p; p++) {
        if (*p == '"' && (p == arrStart || *(p-1) != '\\')) inStr = !inStr;
        if (!inStr) {
          if (*p == '[') depth++;
          else if (*p == ']') { depth--; if (depth == 0) break; }
          else if (*p == ',' && depth == 1) item.commandCount++;
        }
      }
    }
  }

  ctx->count++;
  return true;
}

// ============================================================================
// Prepare Automation Data (called OUTSIDE I2C transaction)
// ============================================================================

void prepareAutomationData() {
  if (!gSettings.automationsEnabled) {
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

  AutoGatherCtx ctx;
  ctx.items = autoRenderData.items;
  ctx.count = 0;
  ctx.maxCount = AUTO_LIST_MAX;

  bool ok = streamParseAutomations(AUTOMATIONS_JSON_FILE, autoGatherCallback, &ctx);

  autoRenderData.count = ctx.count;
  autoRenderData.valid = ok;
  autoRenderData.lastRefresh = now;

  if (autoRenderData.selectedIdx >= autoRenderData.count) {
    autoRenderData.selectedIdx = autoRenderData.count > 0 ? autoRenderData.count - 1 : 0;
  }
}

// ============================================================================
// Display Automations (called INSIDE I2C transaction)
// ============================================================================

void displayAutomations() {
  if (!oledDisplay || !oledConnected) return;

  if (!gSettings.automationsEnabled) {
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

  if (autoRenderData.count == 0) {
    oledDisplay->setTextSize(1);
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 4);
    oledDisplay->println("No automations");
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 16);
    oledDisplay->println("Use CLI to add:");
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 28);
    oledDisplay->println("automationadd ...");
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

  // Scroll offset
  int scrollOffset = 0;
  if (autoRenderData.selectedIdx >= maxVisibleItems) {
    scrollOffset = autoRenderData.selectedIdx - maxVisibleItems + 1;
  }

  // === Left Panel: Automation List ===
  for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < autoRenderData.count; i++) {
    int idx = scrollOffset + i;
    int y = startY + i * itemHeight;
    AutoListItem& item = autoRenderData.items[idx];
    bool isSelected = (idx == autoRenderData.selectedIdx);

    if (isSelected) {
      oledDisplay->fillRect(0, y, listWidth, itemHeight - 1, DISPLAY_COLOR_WHITE);
      oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
    } else {
      oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    }

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

  // === Right Panel: Selected Item Details ===
  if (autoRenderData.count > 0 && autoRenderData.selectedIdx < autoRenderData.count) {
    AutoListItem& sel = autoRenderData.items[autoRenderData.selectedIdx];
    int dy = OLED_CONTENT_START_Y + 2;

    // Type label
    oledDisplay->setCursor(detailX, dy);
    if (strcmp(sel.type, "atTime") == 0)
      oledDisplay->print("@Time");
    else if (strcmp(sel.type, "afterDelay") == 0)
      oledDisplay->print("Delay");
    else if (strcmp(sel.type, "interval") == 0)
      oledDisplay->print("Repeat");
    else
      oledDisplay->print(sel.type);
    dy += 10;

    // Time value
    if (sel.timeStr[0]) {
      oledDisplay->setCursor(detailX, dy);
      oledDisplay->print(sel.timeStr);
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
  if (scrollOffset + maxVisibleItems < autoRenderData.count) {
    oledDisplay->setCursor(78, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 9);
    oledDisplay->print("v");
  }

  // Page indicator in header area
  char pageStr[8];
  snprintf(pageStr, sizeof(pageStr), "%d/%d", autoRenderData.selectedIdx + 1, autoRenderData.count);
  int pageStrWidth = strlen(pageStr) * 6;
  oledDisplay->setCursor(128 - pageStrWidth, 0);
  oledDisplay->print(pageStr);

  // Action feedback overlay
  if (autoActionMsg && (millis() - autoActionMsgTime) < 1500) {
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

static void autoListUp() {
  if (autoRenderData.count == 0) return;
  unsigned long now = millis();
  if (now - autoLastInput < AUTO_DEBOUNCE) return;
  autoLastInput = now;
  if (autoRenderData.selectedIdx > 0)
    autoRenderData.selectedIdx--;
}

static void autoListDown() {
  if (autoRenderData.count == 0) return;
  unsigned long now = millis();
  if (now - autoLastInput < AUTO_DEBOUNCE) return;
  autoLastInput = now;
  if (autoRenderData.selectedIdx < autoRenderData.count - 1)
    autoRenderData.selectedIdx++;
}

static void autoRunSelected() {
  if (autoRenderData.count == 0 || autoRenderData.selectedIdx >= autoRenderData.count) return;
  AutoListItem& sel = autoRenderData.items[autoRenderData.selectedIdx];

  char cmd[48];
  snprintf(cmd, sizeof(cmd), "automationrun id=%ld", sel.id);
  executeOLEDCommand(String(cmd));

  autoActionMsg = "Running...";
  autoActionMsgTime = millis();
}

static void autoToggleSelected() {
  if (autoRenderData.count == 0 || autoRenderData.selectedIdx >= autoRenderData.count) return;
  AutoListItem& sel = autoRenderData.items[autoRenderData.selectedIdx];

  char cmd[48];
  snprintf(cmd, sizeof(cmd), "automation %s id=%ld",
           sel.enabled ? "disable" : "enable", sel.id);
  executeOLEDCommand(String(cmd));

  autoActionMsg = sel.enabled ? "Disabled" : "Enabled";
  autoActionMsgTime = millis();

  // Force data refresh on next frame
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
  extern NavEvents gNavEvents;

  if (gNavEvents.down) {
    autoListDown();
    return true;
  }
  if (gNavEvents.up) {
    autoListUp();
    return true;
  }

  // A = Run selected automation
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    autoRunSelected();
    return true;
  }
  // X = Enable system if disabled, or toggle selected automation
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    if (!gSettings.automationsEnabled) {
      executeOLEDCommand("automation system enable");
      autoForceRefresh = true;
      return true;
    }
    autoToggleSelected();
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

static const OLEDModeEntry sAutomationsModes[] = {
  { OLED_AUTOMATIONS, "Automations", "notify_automation", displayAutomations, nullptr, automationsInputHandler, false, -1 },
};

REGISTER_OLED_MODE_MODULE(sAutomationsModes, sizeof(sAutomationsModes) / sizeof(sAutomationsModes[0]), "Automations");

#endif // ENABLE_OLED_DISPLAY && ENABLE_AUTOMATION
