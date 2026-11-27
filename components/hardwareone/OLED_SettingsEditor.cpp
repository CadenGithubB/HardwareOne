#include "WebServer_Handle.h"
#include "OLED_SettingsEditor.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include <cstring>
#include <strings.h>  // strcasecmp (pick-list current-value match)

#include "i2csensor_seesaw.h"
#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "OLED_MessageText.h"
#include "OLED_TextField.h"

// Row geometry shared by the editor's lists: label fields end before the
// scroll-arrow column; a value keeps a fixed right-aligned column of at most
// ten glyphs, so a long label scrolls under it without moving it.
// The value column's right edge and widest value: 118 / 60 on the 128 px
// row; portrait (64 px) puts values on their own line instead.
#define kSeLabelRight (SCREEN_WIDTH - 10)
#define kSeValueMaxW (SCREEN_WIDTH / 2 - 4)
#include "OLED_UI.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_ESPNow_SensorCore.h"  // fingerprintEditorArg: an emptied fingerprint is 'clear'
#include "System_Settings.h"
#include "System_SettingsEditorCore.h"
#include "System_Utils.h"
#if ENABLE_BLUETOOTH
#include "BLE_Server.h"
#endif
#if ENABLE_G2_GLASSES
#include "G2_Glasses.h"
#endif
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
#include "OLED_RemoteSettings.h"
#endif

// gThermalConnected / gTofConnected come from the sensor headers.
#include "i2csensor_mlx90640.h"
#include "i2csensor_vl53l4cx.h"

// ---------------------------------------------------------------------------
// Thin wrappers over the shared settings-editor core
// (System_SettingsEditorCore). The visibility / editability / enum-options /
// current-value logic moved there so the G2 lens — present on boards that have
// no OLED — can share the exact same code. These wrappers keep the OLED
// editor's original names and semantics so its call sites are unchanged. OLED
// edits INT/BOOL/STRING only (its value path is a single int + slider), which
// is expressed here as SETTINGS_EDIT_MASK_OLED.
static bool isSettingVisible(const SettingEntry* entry) {
  return settingsEditorIsVisible(entry);
}

static bool isEditableEntry(const SettingEntry* entry) {
  return settingsEditorIsEditable(entry, SETTINGS_EDIT_MASK_OLED);
}

static bool entryHasEnumOptions(const SettingEntry* entry) {
  return settingsEditorHasEnumOptions(entry);
}

static int enumOptionCount(const char* options) {
  return settingsEditorEnumCount(options);
}

static bool enumOptionAt(const char* options, int idx,
                         char* valueOut, size_t valueCap,
                         char* labelOut, size_t labelCap) {
  return settingsEditorEnumAt(options, idx, valueOut, valueCap, labelOut, labelCap);
}

static int enumOptionIndexForCurrent(const SettingEntry* entry) {
  return settingsEditorEnumIndexForCurrent(entry);
}

// Global settings editor context
SettingsEditorContext gSettingsEditor;

// ---------------------------------------------------------------------------
// Module source and commit target
// ---------------------------------------------------------------------------
// The remote-settings mode edits the bonded peer's cached module set; every
// other mode edits the local registry. Keyed on the current mode rather than a
// sticky flag, so leaving the remote mode by any route (B, `oledmode`, sleep,
// login) cannot leave the local editor pointed at the peer's modules.
static bool editorIsRemote() {
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
  return currentOledMode == OLED_REMOTE_SETTINGS;
#else
  return false;
#endif
}

static const SettingsModule** editorModules(size_t& count) {
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
  if (editorIsRemote()) return getRemoteSettingsModules(count);
#endif
  return getSettingsModules(count);
}

void settingsEditorShowError(const String& message, uint32_t durationMs) {
  oledToastShow(message.c_str(), durationMs, OledUiIcon::ERROR);
}

// Remote commit: accepted-for-delivery here; the peer's answer reaches the
// screen through OLED_Mode_RemoteSettings' outcome poll. Refusals use the
// shared error toast exactly like a failed local string save.
static bool commitRemoteSettingValue(const SettingEntry* entry, const String& value) {
#if ENABLE_ESPNOW && ENABLE_BONDED_MODE
  String err;
  if (applyRemoteSettingChange(entry, value, err)) return true;
  // The shared short refusal, as for a local save ("Needs an admin" when the
  // local gate refuses the inner setter).
  char text[160];
  oledCommandReplyText(false, err.c_str(), nullptr, "Remote save failed", text, sizeof(text));
  settingsEditorShowError(String(text), 2500);
#else
  (void)entry;
  (void)value;
  settingsEditorShowError("Remote save unavailable", 2500);
#endif
  return false;
}

// Both modes share gSettingsEditor. If it still holds a module that is not in
// the current mode's list (the other mode was left mid-edit by a route other
// than B, or the peer's set was released), start over at the category list
// instead of editing a row of the other device and committing it here.
static void editorDropForeignModule() {
  size_t n = 0;
  const SettingsModule** mods = editorModules(n);
  bool known = (gSettingsEditor.currentModule == nullptr);
  for (size_t i = 0; !known && i < n; i++) {
    known = (mods[i] == gSettingsEditor.currentModule);
  }
  if (!known || (n > 0 && gSettingsEditor.categoryIndex >= (int)n)) {
    initSettingsEditor();
  }
}

// ============================================================================
// Initialization
// ============================================================================

void initSettingsEditor() {
  DEBUG_SYSTEMF("[SettingsEditor] initSettingsEditor called");
  gSettingsEditor.state = SETTINGS_CATEGORY_SELECT;
  gSettingsEditor.categoryIndex = 0;
  gSettingsEditor.itemIndex = 0;
  gSettingsEditor.editValue = 0;
  gSettingsEditor.hasChanges = false;
  gSettingsEditor.currentModule = nullptr;
  gSettingsEditor.currentEntry = nullptr;
  gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
  gSettingsEditor.optionIndex = 0;
  gSettingsEditor.optionCount = 0;
  gSettingsEditor.optionScroll = 0;
  gSettingsEditor.categoryScroll = 0;
  gSettingsEditor.itemScroll = 0;

  // Verify settings modules are registered
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  DEBUG_SYSTEMF("[SettingsEditor] Init complete: %zu modules available", moduleCount);
  if (moduleCount > 0 && modules && modules[0]) {
    DEBUG_SYSTEMF("[SettingsEditor] First module: %s", modules[0]->name);
  }
}

void resetSettingsEditor() {
  initSettingsEditor();
}

// ============================================================================
// Helper Functions
// ============================================================================

// Draw a horizontal slider bar with value indicator
// For bool: shows 0|1 with indicator at current position
// For int: shows min|max with proportional indicator
void drawSettingsSlider(Adafruit_SSD1306* display, int y, int minVal, int maxVal, int currentVal, bool isBool) {
  const int barX = oledPortrait() ? 2 : 10;
  const int barY = y;
  const int barWidth = SCREEN_WIDTH - 2 * barX;  // 108 on the 128 px row
  const int barHeight = 8;
  
  // Draw slider track
  display->drawRect(barX, barY, barWidth, barHeight, DISPLAY_COLOR_WHITE);
  
  // Calculate indicator position
  int range = maxVal - minVal;
  if (range == 0) range = 1;  // Avoid division by zero
  int indicatorX = barX + ((currentVal - minVal) * (barWidth - 4)) / range;
  
  // Draw indicator (filled rectangle)
  display->fillRect(indicatorX, barY + 1, 4, barHeight - 2, DISPLAY_COLOR_WHITE);
  
  // Draw min/max labels
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  display->setCursor(barX, barY + barHeight + 2);
  if (isBool) {
    display->print("0");
    display->setCursor(barX + barWidth - 12, barY + barHeight + 2);
    display->print("1");
  } else {
    char minStr[16];
    snprintf(minStr, sizeof(minStr), "%d", minVal);
    display->print(minStr);
    char maxStr[16];
    snprintf(maxStr, sizeof(maxStr), "%d", maxVal);
    display->setCursor(barX + barWidth - (int)strlen(maxStr) * 6, barY + barHeight + 2);
    display->print(maxStr);
  }

  // Draw current value (centered above bar)
  char valStr[16];
  snprintf(valStr, sizeof(valStr), "%d", currentVal);
  const int valX = barX + (barWidth / 2) - ((int)strlen(valStr) * 3);
  display->setCursor(valX, barY - 10);
  display->setTextSize(1);
  display->print(valStr);
}

void oledDrawSettingValue(Adafruit_SSD1306* display, const char* name, int value, int minVal, int maxVal,
                          bool isBool) {
  if (!display) return;
  const auto g = oled_layout::geometryFor(SCREEN_WIDTH, SCREEN_HEIGHT);
  const int top = g.contentY;
  const char* label = name ? name : "";
  const int cols = oled_layout::cellsIn(g.width);
  const int nameLen = (int)strlen(label);
  display->setTextSize(1);
  display->setTextColor(DISPLAY_COLOR_WHITE);
  // One row that already fits keeps the name on the content start and the
  // slider 14 px below, which is where the value sits on the name row's
  // lower half on the 128-wide screen.
  if (nameLen <= cols) {
    oledTextFieldDraw(display, 0, top, SCREEN_WIDTH, label, DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE, false,
                      nullptr);
    drawSettingsSlider(display, top + 14, minVal, maxVal, value, isBool);
    return;
  }
  const int room = oled_layout::rowsInBand(g, top, 8);
  const auto full = oled_message::wrap<oled_layout::kBandStore>(label, cols, oled_layout::kBandStore);
  // Value row, bar, and the min/max row. Give the name every row under the
  // content start that is not needed for that block, and do not ellipsize
  // while those rows still hold the whole name.
  const int sliderRows = 4;
  int nameCap = room > sliderRows ? room - sliderRows : (room > 0 ? 1 : 0);
  if (full.count > 0 && full.count < nameCap) nameCap = full.count;
  int y = top;
  if (nameCap > 0 && nameLen > 0) y = oledPrintWrapped(display, 0, top, label, nameCap, 8);
  const int barY = y + 10;
  if (barY + 8 <= g.contentBottom) drawSettingsSlider(display, barY, minVal, maxVal, value, isBool);
}

// Width-correct current-value read — implementation moved to
// System_SettingsEditorCore (settingsEditorCurrentValue). Kept under this name
// (non-static) for the OLED editor's existing call sites.
int getSettingCurrentValue(const SettingEntry* entry) {
  return settingsEditorCurrentValue(entry);
}

// Commit a value through the entry's set-command; the remote screen sends it to
// the bonded peer instead. Locally it runs through cmd_exec for the audit log
// (handleSettingCommand's SETTING_STRING case assigns the whole trimmed args
// string, so multi-word values survive), and a refusal or "Error:" reply is
// shown on the shared error toast. Returns false when the save was refused.
static bool setSettingValueStr(const SettingEntry* entry, const String& value) {
  if (!entry || !entry->jsonKey) return false;
  // cmdKey ONLY — see settingsEditorCommandName() for why the jsonKey fallback
  // was removed. No cmdKey means this setting has no set-command, so do nothing
  // rather than firing an unrelated one.
  const char* cmdName = entry->cmdKey;
  if (!cmdName || !cmdName[0]) {
    settingsEditorShowError(String("Not editable here"), 2500);
    return false;
  }
  if (editorIsRemote()) return commitRemoteSettingValue(entry, value);
  char out[128];
  // An emptied master fingerprint goes as 'clear' (no master named): an
  // empty argument would only show the value and report it saved. An emptied
  // esplog field likewise goes as 'clear' (a bare esplog only lists levels).
  const char* arg = espnowsensor::fingerprintEditorArg(cmdName, value.c_str());
  if (!strcasecmp(cmdName, "esplog") && !arg[strspn(arg, " \t\r\n")]) arg = "clear";
  bool ok = executeOledCommandWithResult(String(cmdName) + " " + arg, out, sizeof(out));
  if (!ok) {
    // The shared short refusal ("Needs an admin" for an admin-only setting).
    char text[sizeof(out)];
    oledCommandReplyText(false, out, nullptr, "Save failed", text, sizeof(text));
    settingsEditorShowError(String(text), 2500);
    return false;
  }
  return true;
}

// INT/BOOL twin (slider and INT pick-list): the same checked commit, so a
// refused save shows its reply instead of vanishing into the log.
bool setSettingValue(const SettingEntry* entry, int value) {
  return setSettingValueStr(entry, String(value));
}

// Validate value against min/max
bool validateSettingValue(const SettingEntry* entry, int value, String& errorMsg) {
  if (!entry) return false;
  
  // Check range
  if (entry->minVal != 0 || entry->maxVal != 0) {
    if (value < entry->minVal || value > entry->maxVal) {
      char rangeBuf[48];
      snprintf(rangeBuf, sizeof(rangeBuf), "Value must be %d..%d", entry->minVal, entry->maxVal);
      errorMsg = rangeBuf;
      return false;
    }
  }
  
  return true;
}

// ============================================================================
// Display Functions
// ============================================================================

void displaySettingsEditor() {
  
  if (!oledDisplay) {
    DEBUG_SYSTEMF("[SettingsEditor] oledDisplay is NULL, returning");
    return;
  }

  editorDropForeignModule();

  // Keyboard-edit overlay: while a STRING edit's keyboard is up it owns the
  // whole content area (the global header is suppressed by the keyboard
  // subsystem itself). Draw it and skip the editor chrome entirely.
  if (oledKeyboardDrawIfActive(oledDisplay)) return;

  // Don't call clearDisplay() - the main update loop already cleared the content area
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Get module list
  size_t moduleCount = 0;
  const SettingsModule** modules = editorModules(moduleCount);
  
  
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT: {
      
      // Show category list (header shows "Config", no need for title here)
      oledDisplay->setTextSize(1);
      
      if (moduleCount == 0) {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
        oledDisplay->println("No modules found!");
      } else {
        // Calculate scrolling window (43px content / 10px per item = 4 visible items)
        const int lineHeight = 10;
        const int maxVisibleItems = OLED_CONTENT_HEIGHT / lineHeight;  // 43px / 10px = 4 items
        
        // Calculate scroll offset to keep selection visible (context-owned,
        // reset by initSettingsEditor)
        int& scrollOffset = gSettingsEditor.categoryScroll;
        if (gSettingsEditor.categoryIndex < scrollOffset) {
          scrollOffset = gSettingsEditor.categoryIndex;
        } else if (gSettingsEditor.categoryIndex >= scrollOffset + maxVisibleItems) {
          scrollOffset = gSettingsEditor.categoryIndex - maxVisibleItems + 1;
        }
        
        // Draw visible categories
        int y = OLED_CONTENT_START_Y;
        for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < (int)moduleCount; i++) {
          int itemIdx = scrollOffset + i;
          const bool selected = itemIdx == gSettingsEditor.categoryIndex;
          if (selected) oledDisplay->fillRect(0, y, SCREEN_WIDTH - 3, 10, DISPLAY_COLOR_WHITE);
          oledTextFieldDraw(oledDisplay, 2, y + 1, kSeLabelRight - 2, modules[itemIdx]->name,
                            selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                            selected, &gSettingsEditor);
          y += lineHeight;
        }
        
        // Draw scroll indicators
        if (scrollOffset > 0) {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          oledDisplay->setCursor(SCREEN_WIDTH - 8, OLED_CONTENT_START_Y);
          oledDisplay->print("\x18");  // Up arrow
        }
        if (scrollOffset + maxVisibleItems < (int)moduleCount) {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          oledDisplay->setCursor(SCREEN_WIDTH - 8, OLED_CONTENT_START_Y + (maxVisibleItems - 1) * lineHeight);
          oledDisplay->print("\x19");  // Down arrow
        }
      }
      
      break;
    }
    
    case SETTINGS_ITEM_SELECT: {
      // Show settings list for current category
      // Header breadcrumb already shows "Set>moduleName"
      if (!gSettingsEditor.currentModule) break;
      
      oledDisplay->setTextSize(1);
      
      // Filter to only INT and BOOL settings that are visible
      int visibleCount = 0;
      int visibleIndex = -1;
      
      // Count visible items and find current visible index
      for (size_t i = 0; i < gSettingsEditor.currentModule->count; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if (isEditableEntry(entry)) {
          if (i == (size_t)gSettingsEditor.itemIndex) {
            visibleIndex = visibleCount;
          }
          visibleCount++;
        }
      }
      
      // Calculate scrolling window - content starts below header
      // Portrait: two-line items (label, then the value right-aligned), 16 px each.
      const bool tall = oledPortrait();
      const int itemStartY = OLED_CONTENT_START_Y + (tall ? 0 : 0);
      const int lineHeight = tall ? 16 : 10;
      const int maxVisibleItems = tall ? (oledContentBottom() + 1 - itemStartY) / lineHeight
                                       : OLED_CONTENT_HEIGHT / lineHeight;  // 4 items; 5 in portrait
      
      // Calculate scroll offset to keep selected item visible (context-owned,
      // reset whenever a module is entered)
      int& itemScrollOffset = gSettingsEditor.itemScroll;
      if (visibleIndex < itemScrollOffset) {
        itemScrollOffset = visibleIndex;
      } else if (visibleIndex >= itemScrollOffset + maxVisibleItems) {
        itemScrollOffset = visibleIndex - maxVisibleItems + 1;
      }
      // No editable row under the cursor (visibleIndex -1): window at the top.
      if (itemScrollOffset < 0) itemScrollOffset = 0;

      // Draw visible settings items
      int displayedCount = 0;
      int currentVisibleIdx = 0;
      
      for (size_t i = 0; i < gSettingsEditor.currentModule->count && displayedCount < maxVisibleItems; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        
        // Skip non-editable settings (type, secrets, readOnly, hidden) — the
        // SAME predicate navigation uses, so display and cursor can't desync.
        if (!isEditableEntry(entry)) continue;
        
        // Skip items before scroll offset
        if (currentVisibleIdx < itemScrollOffset) {
          currentVisibleIdx++;
          continue;
        }
        
        int y = itemStartY + displayedCount * lineHeight;
        
        const bool selected = (int)i == gSettingsEditor.itemIndex;
        if (tall) {
          // Line 1 the label (cut, scrolling when selected), line 2 the value
          // right-aligned to x59; the selected item inverts the 61 x 16 block.
          if (selected) oledDisplay->fillRect(0, y, SCREEN_WIDTH - 3, 16, DISPLAY_COLOR_WHITE);
          const uint16_t tallInk = selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
          const char* tallLabel = entry->label ? entry->label : entry->jsonKey;
          const String tallValue = (entry->type == SETTING_STRING) ? *((String*)entry->valuePtr)
                                                                   : String(getSettingCurrentValue(entry));
          oledTextFieldDraw(oledDisplay, 1, y, SCREEN_WIDTH - 5, tallLabel, tallInk, DISPLAY_COLOR_WHITE, selected,
                            &gSettingsEditor);
          int tallW = (int)tallValue.length() * 6 - 1;
          if (tallW > SCREEN_WIDTH - 6) tallW = SCREEN_WIDTH - 6;
          oledTextFieldDraw(oledDisplay, SCREEN_WIDTH - 4 - tallW, y + 8, tallW + 1, tallValue.c_str(), tallInk,
                            DISPLAY_COLOR_WHITE, false, &gSettingsEditor);
          displayedCount++;
          currentVisibleIdx++;
          continue;
        }
        if (selected) oledDisplay->fillRect(0, y - 1, SCREEN_WIDTH - 3, 10, DISPLAY_COLOR_WHITE);
        const uint16_t ink = selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;

        // Label and value keep separate fields: the value in a fixed
        // right-aligned column, the label scrolling under it when selected
        // (OLED_TextField.h). Neither can run into the other or the next row.
        const char* label = entry->label ? entry->label : entry->jsonKey;
        const String value = (entry->type == SETTING_STRING)
                                 ? *((String*)entry->valuePtr)
                                 : String(getSettingCurrentValue(entry));
        int valueW = (int)value.length() * 6;
        if (valueW > kSeValueMaxW) valueW = kSeValueMaxW;
        const int valueX = kSeLabelRight - valueW;
        oledTextFieldDraw(oledDisplay, 2, y, valueX - 6 - 2, label, ink, DISPLAY_COLOR_WHITE,
                          selected, &gSettingsEditor);
        oledTextFieldDraw(oledDisplay, valueX, y, valueW, value.c_str(), ink, DISPLAY_COLOR_WHITE,
                          false, &gSettingsEditor);
        
        displayedCount++;
        currentVisibleIdx++;
      }
      
      // Draw scroll indicators
      if (itemScrollOffset > 0) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(SCREEN_WIDTH - 8, itemStartY);
        oledDisplay->print("\x18");  // Up arrow
      }
      if (itemScrollOffset + maxVisibleItems < visibleCount) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(SCREEN_WIDTH - 8, itemStartY + (maxVisibleItems - 1) * lineHeight);
        oledDisplay->print("\x19");  // Down arrow
      }
      
      break;
    }
    
    case SETTINGS_VALUE_EDIT: {
      // Show value editor: slider (INT/BOOL), pick-list (enum options), or
      // nothing (keyboard edits early-returned above while active).
      // Header breadcrumb already shows "Set>moduleName"
      if (!gSettingsEditor.currentEntry) break;

      oledDisplay->setTextSize(1);
      const char* settingName = gSettingsEditor.currentEntry->label ? gSettingsEditor.currentEntry->label
                                                                    : gSettingsEditor.currentEntry->jsonKey;

      if (gSettingsEditor.editKind == SETTINGS_EDIT_PICKLIST) {
      oledTextFieldDraw(oledDisplay, 0, OLED_CONTENT_START_Y, SCREEN_WIDTH, settingName, DISPLAY_COLOR_WHITE,
                        DISPLAY_COLOR_WHITE, false, &gSettingsEditor);
        // Option rows below the label — same invert-bar + arrow chrome as the
        // category list, one row per parsed option label. Scroll offset lives
        // in the context (not a static local) so it can't leak across entries.
        const int lineHeight = 10;
        const int listStartY = OLED_CONTENT_START_Y + lineHeight;
        const int maxVisible = (OLED_CONTENT_HEIGHT - lineHeight) / lineHeight;  // 3 rows

        if (gSettingsEditor.optionIndex < gSettingsEditor.optionScroll) {
          gSettingsEditor.optionScroll = gSettingsEditor.optionIndex;
        } else if (gSettingsEditor.optionIndex >= gSettingsEditor.optionScroll + maxVisible) {
          gSettingsEditor.optionScroll = gSettingsEditor.optionIndex - maxVisible + 1;
        }

        char optLabel[24];
        int y = listStartY;
        for (int i = 0; i < maxVisible &&
                        (gSettingsEditor.optionScroll + i) < gSettingsEditor.optionCount; i++) {
          const int idx = gSettingsEditor.optionScroll + i;
          if (!enumOptionAt(gSettingsEditor.currentEntry->optionsText(), idx,
                            nullptr, 0, optLabel, sizeof(optLabel))) {
            break;
          }
          const bool selected = idx == gSettingsEditor.optionIndex;
          if (selected) oledDisplay->fillRect(0, y, SCREEN_WIDTH - 3, 10, DISPLAY_COLOR_WHITE);
          oledTextFieldDraw(oledDisplay, 2, y + 1, kSeLabelRight - 2, optLabel,
                            selected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE, DISPLAY_COLOR_WHITE,
                            selected, &gSettingsEditor);
          y += lineHeight;
        }
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        if (gSettingsEditor.optionScroll > 0) {
          oledDisplay->setCursor(SCREEN_WIDTH - 8, listStartY);
          oledDisplay->print("\x18");
        }
        if (gSettingsEditor.optionScroll + maxVisible < gSettingsEditor.optionCount) {
          oledDisplay->setCursor(SCREEN_WIDTH - 8, listStartY + (maxVisible - 1) * lineHeight);
          oledDisplay->print("\x19");
        }
        break;
      }

      bool isBool = (gSettingsEditor.currentEntry->type == SETTING_BOOL);
      oledDrawSettingValue(oledDisplay, settingName, gSettingsEditor.editValue, gSettingsEditor.currentEntry->minVal,
                           gSettingsEditor.currentEntry->maxVal, isBool);

      // Show change indicator
      if (gSettingsEditor.hasChanges) {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 10);
        oledDisplay->print("* Modified");
      }

      break;
    }
  }
  
  // Don't call display() here - let updateOledDisplay() render footer and display in same frame
}

// ============================================================================
// Input Handling
// ============================================================================

bool handleSettingsEditorInput(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  editorDropForeignModule();

  // ---- Keyboard-edit completion poll (CLIInput idiom) ----
  // While the keyboard overlay is ACTIVE this handler is never invoked (the
  // central dispatch in OLED_Utils.cpp consumes all input). Once X/START
  // completes or B cancels, active goes false and the NEXT event lands here.
  if (gSettingsEditor.state == SETTINGS_VALUE_EDIT &&
      gSettingsEditor.editKind == SETTINGS_EDIT_KEYBOARD) {
    if (oledKeyboardIsCompleted()) {
      String text = oledKeyboardGetText();
      text.trim();
      oledKeyboardReset();
      if (text.length() == 0) {
        // Committing "" would send "<cmd>" with empty args, which SHOWS the
        // current value instead of clearing it — a silent no-op. Refuse.
        settingsEditorShowError("Empty - not saved", 2000);
      } else if (setSettingValueStr(gSettingsEditor.currentEntry, text)) {
        DEBUG_SYSTEMF("[SettingsEditor] Saved %s = \"%s\" (keyboard)",
                      gSettingsEditor.currentEntry->jsonKey, text.c_str());
      }
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
      return true;
    }
    if (oledKeyboardIsCancelled()) {
      oledKeyboardReset();
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
      return true;
    }
    return false;  // keyboard still active — central dispatch owns input
  }
  // Canonical-signal pattern: read gNavEvents.up/down/left/right (set by the
  // input layer for ALL devices — joystick threshold-crossings with built-in
  // auto-repeat, ANO wheel sign, ANO dpad edges). Don't do our own deadzone +
  // auto-repeat on raw deltaX/deltaY — that was a joystick-only assumption
  // that broke for the rotary encoder, and it duplicated cadence logic the
  // input layer already provides for the joystick path.
  //
  // For fine value adjustment in edit mode, ALSO read gNavEvents.wheelDelta
  // (the canonical rotary signal): each detent = one unit, sign-correct.
  // The joystick path still works via gNavEvents.left/right.
  bool handled = false;

  // ---- List navigation ----
  if (gNavEvents.up) {
    settingsEditorUp();
    handled = true;
  } else if (gNavEvents.down) {
    settingsEditorDown();
    handled = true;
  }

  // ---- Value adjustment in edit mode ----
  // Gamepad: LEFT/RIGHT joystick deflection nudges value — the joystick has
  // no dedicated "value adjust" axis, so we steal LEFT/RIGHT for it.
  // ANO encoder: wheelDelta is the canonical adjust signal; LEFT is reserved
  // for B-back. Without the compile-time gate, ANO LEFT would decrement the
  // value AND exit edit mode in the same tick, persisting an unwanted
  // mutation. Gating on !ENABLE_ANO_ENCODER keeps both backends clean.
  if (gSettingsEditor.state == SETTINGS_VALUE_EDIT &&
      gSettingsEditor.editKind == SETTINGS_EDIT_PICKLIST &&
      gNavEvents.wheelDelta != 0) {
    // ANO wheel moves the pick-list cursor (same canonical signal the slider
    // uses for value adjust). Gamepad users navigate with up/down.
    if (gNavEvents.wheelDelta > 0) settingsEditorDown();
    else                           settingsEditorUp();
    handled = true;
  }

  if (gSettingsEditor.state == SETTINGS_VALUE_EDIT &&
      gSettingsEditor.editKind == SETTINGS_EDIT_SLIDER) {
    int adjust = 0;
#if !ENABLE_ANO_ENCODER
    if (gNavEvents.right)             adjust = +1;
    else if (gNavEvents.left)         adjust = -1;
    else
#endif
    if (gNavEvents.wheelDelta != 0) adjust = (gNavEvents.wheelDelta > 0) ? +1 : -1;

    if (adjust > 0) {
      if (gSettingsEditor.editValue < gSettingsEditor.currentEntry->maxVal) {
        gSettingsEditor.editValue++;
        gSettingsEditor.hasChanges = true;
        handled = true;
      }
    } else if (adjust < 0) {
      if (gSettingsEditor.editValue > gSettingsEditor.currentEntry->minVal) {
        gSettingsEditor.editValue--;
        gSettingsEditor.hasChanges = true;
        handled = true;
      }
    }
  }

  // ---- Button actions (unchanged) ----
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    settingsEditorSelect();
    handled = true;
  }

  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (gSettingsEditor.state == SETTINGS_CATEGORY_SELECT) {
      // At top level — let caller handle exit to menu (returns false → menu back).
      return false;
    } else {
      settingsEditorBack();
      handled = true;
    }
  }

  return handled;
}

// ============================================================================
// Navigation Functions
// ============================================================================

void settingsEditorUp() {
  size_t moduleCount = 0;
  const SettingsModule** modules = editorModules(moduleCount);
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      if (gSettingsEditor.categoryIndex > 0) {
        gSettingsEditor.categoryIndex--;
      } else {
        gSettingsEditor.categoryIndex = moduleCount - 1;  // Wrap to bottom
      }
      break;
      
    case SETTINGS_ITEM_SELECT:
      if (!gSettingsEditor.currentModule) break;
      
      // Find previous INT/BOOL visible setting
      for (int i = gSettingsEditor.itemIndex - 1; i >= 0; i--) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if (isEditableEntry(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      
      // Wrap to last INT/BOOL visible setting
      for (int i = gSettingsEditor.currentModule->count - 1; i >= 0; i--) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if (isEditableEntry(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      break;

    case SETTINGS_VALUE_EDIT:
      // Pick-list: up moves the option cursor (wraps). Slider keeps the
      // original left/right-only convention; keyboard never reaches here.
      if (gSettingsEditor.editKind == SETTINGS_EDIT_PICKLIST &&
          gSettingsEditor.optionCount > 0) {
        gSettingsEditor.optionIndex =
            (gSettingsEditor.optionIndex == 0) ? gSettingsEditor.optionCount - 1
                                               : gSettingsEditor.optionIndex - 1;
      }
      break;
  }
}

void settingsEditorDown() {
  size_t moduleCount = 0;
  const SettingsModule** modules = editorModules(moduleCount);
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      if (gSettingsEditor.categoryIndex < (int)moduleCount - 1) {
        gSettingsEditor.categoryIndex++;
      } else {
        gSettingsEditor.categoryIndex = 0;  // Wrap to top
      }
      break;
      
    case SETTINGS_ITEM_SELECT:
      if (!gSettingsEditor.currentModule) break;
      
      // Find next INT/BOOL visible setting
      for (size_t i = gSettingsEditor.itemIndex + 1; i < gSettingsEditor.currentModule->count; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if (isEditableEntry(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      
      // Wrap to first INT/BOOL visible setting
      for (size_t i = 0; i < gSettingsEditor.currentModule->count; i++) {
        const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
        if (isEditableEntry(entry)) {
          gSettingsEditor.itemIndex = i;
          return;
        }
      }
      break;

    case SETTINGS_VALUE_EDIT:
      // Pick-list: down moves the option cursor (wraps).
      if (gSettingsEditor.editKind == SETTINGS_EDIT_PICKLIST &&
          gSettingsEditor.optionCount > 0) {
        gSettingsEditor.optionIndex =
            (gSettingsEditor.optionIndex + 1) % gSettingsEditor.optionCount;
      }
      break;
  }
}

void settingsEditorSelect() {
  size_t moduleCount = 0;
  const SettingsModule** modules = editorModules(moduleCount);
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      // Enter selected category
      if (gSettingsEditor.categoryIndex < (int)moduleCount) {
        gSettingsEditor.currentModule = modules[gSettingsEditor.categoryIndex];
        gSettingsEditor.itemIndex = 0;
        gSettingsEditor.itemScroll = 0;

        // Find first editable setting (this scan previously used a bare
        // INT/BOOL test WITHOUT isSettingVisible — the shared predicate also
        // fixes that latent desync).
        for (size_t i = 0; i < gSettingsEditor.currentModule->count; i++) {
          const SettingEntry* entry = &gSettingsEditor.currentModule->entries[i];
          if (isEditableEntry(entry)) {
            gSettingsEditor.itemIndex = i;
            break;
          }
        }
        
        gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      }
      break;
      
    case SETTINGS_ITEM_SELECT:
      // Enter value editor for selected setting
      if (!gSettingsEditor.currentModule) break;
      if (gSettingsEditor.itemIndex >= (int)gSettingsEditor.currentModule->count) break;
      
      gSettingsEditor.currentEntry = &gSettingsEditor.currentModule->entries[gSettingsEditor.itemIndex];

      // Defense in depth — navigation should never land here on a
      // non-editable entry now that everything shares isEditableEntry.
      if (!isEditableEntry(gSettingsEditor.currentEntry)) {
        settingsEditorShowError("Not editable", 2000);
        break;
      }

      // Dispatch on edit kind: enum pick-list (INT or STRING with an options
      // string), keyboard (plain STRING), or the original slider (INT/BOOL).
      if (entryHasEnumOptions(gSettingsEditor.currentEntry) &&
          gSettingsEditor.currentEntry->type != SETTING_BOOL) {
        gSettingsEditor.editKind = SETTINGS_EDIT_PICKLIST;
        gSettingsEditor.optionCount = enumOptionCount(gSettingsEditor.currentEntry->optionsText());
        gSettingsEditor.optionIndex = enumOptionIndexForCurrent(gSettingsEditor.currentEntry);
        gSettingsEditor.optionScroll = 0;
        gSettingsEditor.hasChanges = false;
        gSettingsEditor.state = SETTINGS_VALUE_EDIT;
      } else if (gSettingsEditor.currentEntry->type == SETTING_STRING) {
        const String& cur = *((String*)gSettingsEditor.currentEntry->valuePtr);
        if ((int)cur.length() > OLED_KEYBOARD_MAX_LENGTH) {
          // The keyboard buffer would silently clip the prefill and a save
          // would corrupt the value — refuse rather than truncate.
          settingsEditorShowError("Too long for OLED edit", 2500);
          break;
        }
        gSettingsEditor.editKind = SETTINGS_EDIT_KEYBOARD;
        const char* label = gSettingsEditor.currentEntry->label
                                ? gSettingsEditor.currentEntry->label
                                : gSettingsEditor.currentEntry->jsonKey;
        // isEditableEntry() excludes SettingEntry::isSecret, so every string
        // that reaches this branch is an explicitly non-secret setting.
        oledKeyboardInit(label, cur.c_str(), OLED_KEYBOARD_MAX_LENGTH,
                         OledKeyboardDictationPolicy::ALLOW_PLAINTEXT);
        gSettingsEditor.hasChanges = false;
        gSettingsEditor.state = SETTINGS_VALUE_EDIT;
      } else {
        gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
        gSettingsEditor.editValue = getSettingCurrentValue(gSettingsEditor.currentEntry);
        gSettingsEditor.hasChanges = false;
        gSettingsEditor.state = SETTINGS_VALUE_EDIT;
      }
      break;
      
    case SETTINGS_VALUE_EDIT: {
      // Save value
      if (!gSettingsEditor.currentEntry) break;

      if (gSettingsEditor.editKind == SETTINGS_EDIT_PICKLIST) {
        // Commit the highlighted option's VALUE token (never the label).
        char val[48];
        if (!enumOptionAt(gSettingsEditor.currentEntry->optionsText(),
                          gSettingsEditor.optionIndex, val, sizeof(val),
                          nullptr, 0)) {
          break;
        }
        bool ok;
        if (gSettingsEditor.currentEntry->type == SETTING_STRING) {
          ok = setSettingValueStr(gSettingsEditor.currentEntry, String(val));
        } else {
          ok = setSettingValue(gSettingsEditor.currentEntry, atoi(val));
        }
        if (ok) {
          DEBUG_SYSTEMF("[SettingsEditor] Saved %s = %s (pick-list)",
                        gSettingsEditor.currentEntry->jsonKey, val);
          gSettingsEditor.state = SETTINGS_ITEM_SELECT;
        }
        break;
      }

      if (gSettingsEditor.editKind == SETTINGS_EDIT_KEYBOARD) {
        // A never reaches here while the keyboard is up (central dispatch
        // consumes all input); commit happens in handleSettingsEditorInput's
        // completion poll. If we somehow land here, just bail to the list.
        gSettingsEditor.state = SETTINGS_ITEM_SELECT;
        break;
      }

      // Slider (INT/BOOL) — original path.
      String errorMsg;
      if (!validateSettingValue(gSettingsEditor.currentEntry, gSettingsEditor.editValue, errorMsg)) {
        settingsEditorShowError(errorMsg, 2000);
        break;
      }

      // Apply via command system — generates [CMD] audit line and persists.
      // A refused save stays in the value editor with the reply on the error line.
      if (!setSettingValue(gSettingsEditor.currentEntry, gSettingsEditor.editValue)) break;

      DEBUG_SYSTEMF("[SettingsEditor] Saved %s = %d",
                    gSettingsEditor.currentEntry->jsonKey, gSettingsEditor.editValue);

      // Return to item select
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.hasChanges = false;
      break;
    }
  }
}

// Open settings editor directly to a specific module by name
// Returns true if module was found and editor was opened
bool openSettingsEditorForModule(const char* moduleName) {
  if (!moduleName) return false;
  
  size_t moduleCount = 0;
  const SettingsModule** modules = editorModules(moduleCount);
  
  for (size_t i = 0; i < moduleCount; i++) {
    if (modules[i] && strcmp(modules[i]->name, moduleName) == 0) {
      // Found the module - set up editor to start there
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.categoryIndex = i;
      gSettingsEditor.currentModule = modules[i];
      gSettingsEditor.itemIndex = 0;
      gSettingsEditor.editValue = 0;
      gSettingsEditor.hasChanges = false;
      gSettingsEditor.currentEntry = nullptr;
      gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
      gSettingsEditor.optionIndex = 0;
      gSettingsEditor.optionCount = 0;
      gSettingsEditor.optionScroll = 0;
      gSettingsEditor.categoryScroll = 0;
      gSettingsEditor.itemScroll = 0;

      DEBUG_SYSTEMF("[SettingsEditor] Opened module: %s", moduleName);
      return true;
    }
  }
  
  DEBUG_SYSTEMF("[SettingsEditor] Module not found: %s", moduleName);
  return false;
}

void settingsEditorBack() {
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      // Exit settings editor (handled by caller)
      break;
      
    case SETTINGS_ITEM_SELECT:
      // Return to category select
      gSettingsEditor.state = SETTINGS_CATEGORY_SELECT;
      gSettingsEditor.currentModule = nullptr;
      break;
      
    case SETTINGS_VALUE_EDIT:
      // Cancel edit and return to item select. (Keyboard edits cancel via the
      // keyboard's own B handling + the completion poll, not through here.)
      gSettingsEditor.state = SETTINGS_ITEM_SELECT;
      gSettingsEditor.hasChanges = false;
      gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
      break;
  }
}

// ============================================================================
// Settings Mode Registration (merged from oled_settings_mode.cpp)
// ============================================================================

// Forward declaration for registerOledModes
void registerOledModes(const OledModeEntry* modes, size_t count);

// Display handler for settings mode
void displaySettingsMode() {
  DEBUG_SYSTEMF("[SettingsMode] displaySettingsMode called!!!");
  
  // Initialize on first entry if needed
  static bool initialized = false;
  if (!initialized) {
    DEBUG_SYSTEMF("[SettingsMode] Initializing settings editor");
    initSettingsEditor();
    initialized = true;
  }
  
  DEBUG_SYSTEMF("[SettingsMode] Calling displaySettingsEditor");
  displaySettingsEditor();
  DEBUG_SYSTEMF("[SettingsMode] displaySettingsEditor returned");
}

// Input handler for settings mode
bool handleSettingsInput(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Check if we should exit back to menu
  if (gSettingsEditor.state == SETTINGS_CATEGORY_SELECT && 
      INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    // Let default handler take us back to menu
    return false;
  }
  
  // Otherwise, let settings editor handle input
  return handleSettingsEditorInput(deltaX, deltaY, newlyPressed);
}

// Availability check - always available
bool isSettingsAvailable(String* outReason) {
  return true;
}

// Settings mode entry
static const OledModeEntry kSettingsModeEntry = {
  OLED_SETTINGS,
  "Settings",
  "settings",  // Icon name
  displaySettingsMode,
  isSettingsAvailable,
  handleSettingsInput,
  true,   // Show in menu
  100,    // Menu order (near end)
  nullptr // hints
};

// Settings modes array
// Columns: mode, name, iconName, displayFunc, availFunc, inputFunc, showInMenu, menuOrder, hints
static const OledModeEntry kSettingsOledModes[] = {
  kSettingsModeEntry
};

// Auto-register settings mode
REGISTER_OLED_MODE_MODULE(kSettingsOledModes, sizeof(kSettingsOledModes) / sizeof(kSettingsOledModes[0]), "Settings");

// ============================================================================
// Quick Settings Mode (merged from oled_quick_settings.cpp)
// ============================================================================

#include "System_Net.h"
#if ENABLE_WIFI
#include <WiFi.h>
#include "System_WiFi.h"  // wifiRadioDisplayOn — the Radio row
#endif

// ============================================================================
// Quick Settings - Dynamic item registry based on compile flags
// ============================================================================

typedef bool (*QuickGetStateFunc)();
typedef void (*QuickToggleFunc)();

struct QuickSettingsItem {
  const char* name;
  QuickGetStateFunc getState;
  QuickToggleFunc toggle;
};

#define MAX_QUICK_ITEMS 8
EXT_RAM_BSS_ATTR static QuickSettingsItem quickItems[MAX_QUICK_ITEMS];
static int sQuickItemCount = 0;
static bool sQuickItemsInitialized = false;

static int sQuickSelectedItem = 0;

static char sQuickStatusMsg[32] = "";

static void setQuickStatus(const char* msg, unsigned long durationMs = 2000) {
  oledToastShow(msg, durationMs, OledUiIcon::SUCCESS);
}

static void addQuickItem(const char* name, QuickGetStateFunc getState, QuickToggleFunc toggle) {
  if (sQuickItemCount < MAX_QUICK_ITEMS) {
    quickItems[sQuickItemCount] = { name, getState, toggle };
    sQuickItemCount++;
  }
}

// Every row's command runs without holding up the frame loop: radiopower,
// openwifi and openble take from a fraction of a second to many seconds (on
// the P4 each is a series of RPCs to the C6). The row shows its new state once
// the command lands; until then the status line says it is running.
static void quickCommandDone(bool ok, const void* okStatus) {
  sQuickStatusMsg[0] = '\0';
  if (ok) setQuickStatus(static_cast<const char*>(okStatus));
}
static void runQuickCommand(const char* command, const char* okStatus) {
  if (oledRunCommandAsync(command, quickCommandDone, okStatus)) {
    snprintf(sQuickStatusMsg, sizeof(sQuickStatusMsg), "Working...");
    oledMarkDirty();
  }
}

// --- Radio power (airplane mode) ---
// The RADIO axis: whether the 2.4GHz radio is powered up at all (ON whenever
// WiFi is connecting/connected OR ESP-NOW holds it). Separate from the WiFi
// CONNECTION row below. Toggling off powers the whole radio down (ESP-NOW too);
// runtime only — the radio returns to its configured state on the next boot.
#if ENABLE_WIFI
static bool getQuickRadioState() {
  return wifiRadioDisplayOn();  // read every frame
}
static void toggleQuickRadio() {
  if (wifiRadioOn()) {
    runQuickCommand("radiopower off", "Radio OFF");
  } else {
    runQuickCommand("radiopower on", "Radio ON");
  }
}
#endif

// --- WiFi ---
#if ENABLE_WIFI
// State reflects the CONNECTION, not the radio mode: ESP-NOW holds the radio in
// WIFI_AP_STA (mode never NULL), so keying on getMode() left this stuck ON.
static bool getQuickWifiState() {
  return wifiStaJoined();
}
static void toggleQuickWifi() {
  // Can't connect a network with the radio powered down — mirror the HTTP row's
  // "Need WiFi first!" refusal instead of firing openwifi against a dead radio.
  if (!wifiRadioOn()) {
    oledToastShow("Radio is off. Turn it on first.", 8000, OledUiIcon::ERROR);
    return;
  }
  if (wifiStaJoined()) {
    runQuickCommand("closewifi", "WiFi OFF");
  } else {
    runQuickCommand("openwifi --best", "WiFi ON");
  }
}
#endif

// --- Bluetooth ---
#if ENABLE_BLUETOOTH
static bool getQuickBluetoothState() {
  return bleSubsystemActive();
}
static void bluetoothToggleConfirmedQuick(void* userData) {
  (void)userData;
  const bool wasOn = bleSubsystemActive();
  const char* cmd = oledBluetoothToggleCommand();
  if (cmd) runQuickCommand(cmd, wasOn ? "Bluetooth OFF" : "Bluetooth ON");
}
static void toggleQuickBluetooth() {
  if (bleSubsystemActive()) {
    oledConfirmRequest("Stop Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr, false);
  } else {
    oledConfirmRequest("Start Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr);
  }
}
#endif

// --- HTTP Server ---
#if ENABLE_HTTP_SERVER
static bool getQuickHttpState() {
  return (server != nullptr);
}
static void httpToggleConfirmedQuick(void* userData) {
  (void)userData;
  if (server != nullptr) {
    runQuickCommand("closehttp", "HTTP OFF");
  } else {
    if (!netUp()) {
      oledToastShow("Connect Wi-Fi first.", 8000, OledUiIcon::ERROR);
      return;
    }
    runQuickCommand("openhttp", "HTTP ON");
  }
}
static void toggleQuickHttp() {
  if (server != nullptr) {
    oledConfirmRequest("Stop HTTP?", nullptr, httpToggleConfirmedQuick, nullptr, false);
  } else {
    if (!netUp()) {
      oledToastShow("Connect Wi-Fi first.", 8000, OledUiIcon::ERROR);
      return;
    }
    oledConfirmRequest("Start HTTP?", nullptr, httpToggleConfirmedQuick, nullptr);
  }
}
#endif

static void initQuickItems() {
  if (sQuickItemsInitialized) return;
  sQuickItemsInitialized = true;
  sQuickItemCount = 0;
#if ENABLE_WIFI
  addQuickItem("Radio", getQuickRadioState, toggleQuickRadio);  // radio power (airplane mode) — above WiFi
  addQuickItem("WiFi", getQuickWifiState, toggleQuickWifi);      // network connection
#endif
#if ENABLE_BLUETOOTH
  addQuickItem("Bluetooth", getQuickBluetoothState, toggleQuickBluetooth);
#endif
#if ENABLE_HTTP_SERVER
  addQuickItem("HTTP Server", getQuickHttpState, toggleQuickHttp);
#endif
}

// Display function
void displayQuickSettings() {
  if (!oledDisplay || !oledConnected) return;
  initQuickItems();
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);

  // No separator line here: the header already draws its own boundary line
  // (OLED_HEADER_HEIGHT-1); a second line at OLED_CONTENT_START_Y rendered as a
  // doubled edge right under it.

  if (sQuickItemCount == 0) {
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 14);
    oledDisplay->print("No toggles available");
  } else {
    // Layout adapts to the row count so it always fits the 43px content area.
    // <=3 toggles keep the original +5/step-13/12px-highlight look (last
    // highlight Y=40..51, 1px above the footer boundary). 4+ toggles (Radio +
    // WiFi + Bluetooth + HTTP) shrink the step/highlight to stay on the 64px
    // panel: step = (CONTENT_HEIGHT-3)/count (e.g. 10 for 4 rows).
    const bool tight = (sQuickItemCount > 3);
    const int stepPx  = tight ? ((OLED_CONTENT_HEIGHT - 3) / sQuickItemCount) : 13;
    const int hiliteH = tight ? (stepPx - 1) : 12;
    int yPos = OLED_CONTENT_START_Y + (tight ? 3 : 5);
    for (int i = 0; i < sQuickItemCount; i++) {
      bool isSelected = (i == sQuickSelectedItem);
      bool isEnabled = quickItems[i].getState ? quickItems[i].getState() : false;

      if (isSelected) {
        oledDisplay->fillRect(0, yPos - 2, SCREEN_WIDTH - 3, hiliteH, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      }

      // The name field ends before the fixed ON/OFF column (OLED_TextField.h).
      const uint16_t ink = isSelected ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
      // The state column: x90 on the 128 px row; portrait keeps 3 cells ("ON"/"OFF").
      const int stateX = oledPortrait() ? SCREEN_WIDTH - 22 : 90;
      oledTextFieldDraw(oledDisplay, oledPortrait() ? 1 : 4, yPos, stateX - 6 - (oledPortrait() ? 1 : 4),
                        quickItems[i].name, ink, ink, isSelected, quickItems);

      oledDisplay->setCursor(stateX, yPos);
      if (oledPortrait()) {
        oledDisplay->print(isEnabled ? "ON" : "OFF");
        yPos += stepPx;
        continue;
      }
      oledDisplay->print(isEnabled ? "[ON]" : "[OFF]");

      yPos += stepPx;
    }
  }
  
  // Ongoing work is screen state; terminal results use shared feedback.
  if (sQuickStatusMsg[0]) {
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_HEIGHT - 10);
    oledDisplay->print(sQuickStatusMsg);
  }
}

// Input handler
bool quickSettingsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initQuickItems();
  if (sQuickItemCount == 0) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      requestOledMode(popOledMode(), "quicksettings.back.empty", false, /*isBackNav=*/true);
      return true;
    }
    return false;
  }
  
  bool handled = false;
  
  if (gNavEvents.up) {
    sQuickSelectedItem = (sQuickSelectedItem - 1 + sQuickItemCount) % sQuickItemCount;
    handled = true;
  } else if (gNavEvents.down) {
    sQuickSelectedItem = (sQuickSelectedItem + 1) % sQuickItemCount;
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (sQuickSelectedItem >= 0 && sQuickSelectedItem < sQuickItemCount && quickItems[sQuickSelectedItem].toggle) {
      quickItems[sQuickSelectedItem].toggle();
    }
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    requestOledMode(popOledMode(), "quicksettings.back", false, /*isBackNav=*/true);
    handled = true;
  }
  
  return handled;
}

// Note: Quick settings mode is registered directly in oled_display.cpp
// to ensure it's always linked and available (accessed via SELECT button)

#endif // ENABLE_OLED_DISPLAY
