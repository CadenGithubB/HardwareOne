#include "WebServer_Handle.h"
#include "OLED_SettingsEditor.h"

#if ENABLE_OLED_DISPLAY

#include <Adafruit_SSD1306.h>
#include <cstring>
#include <strings.h>  // strcasecmp (pick-list current-value match)

#include "i2csensor_seesaw.h"
#include "OLED_Display.h"
#include "OLED_Utils.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Settings.h"
#include "System_SettingsEditorCore.h"
#include "System_Utils.h"

// gThermalConnected / gTofConnected come from the sensor headers.
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif

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
  gSettingsEditor.errorMessage = "";
  gSettingsEditor.errorDisplayUntil = 0;
  gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
  gSettingsEditor.optionIndex = 0;
  gSettingsEditor.optionCount = 0;
  gSettingsEditor.optionScroll = 0;

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
  const int barX = 10;
  const int barY = y;
  const int barWidth = 108;  // Leave room for value text
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
  display->setCursor(barX, barY + barHeight + 2);
  if (isBool) {
    display->print("0");
  } else {
    display->print(minVal);
  }
  
  display->setCursor(barX + barWidth - 12, barY + barHeight + 2);
  if (isBool) {
    display->print("1");
  } else {
    // Right-align max value
    String maxStr = String(maxVal);
    display->setCursor(barX + barWidth - (maxStr.length() * 6), barY + barHeight + 2);
    display->print(maxVal);
  }
  
  // Draw current value (centered above bar)
  String valStr = String(currentVal);
  int valX = barX + (barWidth / 2) - (valStr.length() * 3);
  display->setCursor(valX, barY - 10);
  display->setTextSize(1);
  display->print(currentVal);
}

// Width-correct current-value read — implementation moved to
// System_SettingsEditorCore (settingsEditorCurrentValue). Kept under this name
// (non-static) for the OLED editor's existing call sites.
int getSettingCurrentValue(const SettingEntry* entry) {
  return settingsEditorCurrentValue(entry);
}

// Set value to setting entry — routes through command system for audit logging
void setSettingValue(const SettingEntry* entry, int value) {
  if (!entry || !entry->jsonKey) return;

  // cmdKey ONLY — see settingsEditorCommandName() for why the jsonKey fallback
  // was removed. No cmdKey means this setting has no set-command, so do nothing
  // rather than firing an unrelated one.
  const char* cmdName = entry->cmdKey;
  if (!cmdName || !cmdName[0]) {
    gSettingsEditor.errorMessage = String("Not editable here");
    gSettingsEditor.errorDisplayUntil = millis() + 2500;
    return;
  }
  String cmd = String(cmdName) + " " + String(value);
  executeOLEDCommand(cmd);
}

// String twin of setSettingValue — same cmdKey-or-jsonKey resolution, same
// cmd_exec path (handleSettingCommand's SETTING_STRING case assigns the whole
// trimmed args string, so multi-word values survive). Uses the WithResult
// variant so an "Error:" reply surfaces via the editor's toast instead of
// vanishing into the log. Returns false when the command reported failure.
static bool setSettingValueStr(const SettingEntry* entry, const String& value) {
  if (!entry || !entry->jsonKey) return false;
  const char* cmdName = entry->cmdKey;   // cmdKey ONLY — no jsonKey fallback
  if (!cmdName || !cmdName[0]) {
    gSettingsEditor.errorMessage = String("Not editable here");
    gSettingsEditor.errorDisplayUntil = millis() + 2500;
    return false;
  }
  char out[128];
  bool ok = executeOLEDCommandWithResult(String(cmdName) + " " + value, out, sizeof(out));
  if (!ok || strncmp(out, "Error", 5) == 0) {
    gSettingsEditor.errorMessage = (out[0] != '\0') ? String(out) : String("Save failed");
    gSettingsEditor.errorDisplayUntil = millis() + 2500;
    return false;
  }
  return true;
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
  
  // Keyboard-edit overlay: while a STRING edit's keyboard is up it owns the
  // whole content area (the global header is suppressed by the keyboard
  // subsystem itself). Draw it and skip the editor chrome entirely.
  if (oledKeyboardDrawIfActive(oledDisplay)) return;

  // Don't call clearDisplay() - the main update loop already cleared the content area
  oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
  
  // Get module list
  size_t moduleCount = 0;
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
  
  
  // Display error message if active
  if (millis() < gSettingsEditor.errorDisplayUntil) {
    DEBUG_SYSTEMF("[SettingsEditor] Showing error: %s", gSettingsEditor.errorMessage.c_str());
    oledDisplay->setTextSize(1);
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
    oledDisplay->println("ERROR:");
    oledDisplay->setCursor(0, OLED_CONTENT_START_Y + 10);
    oledDisplay->println(gSettingsEditor.errorMessage);
    // Note: Don't call display() here - main render loop handles it via displayUpdate()
    return;
  }
  
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
        
        // Calculate scroll offset to keep selection visible
        static int scrollOffset = 0;
        if (gSettingsEditor.categoryIndex < scrollOffset) {
          scrollOffset = gSettingsEditor.categoryIndex;
        } else if (gSettingsEditor.categoryIndex >= scrollOffset + maxVisibleItems) {
          scrollOffset = gSettingsEditor.categoryIndex - maxVisibleItems + 1;
        }
        
        // Draw visible categories
        int y = OLED_CONTENT_START_Y;
        for (int i = 0; i < maxVisibleItems && (scrollOffset + i) < (int)moduleCount; i++) {
          int itemIdx = scrollOffset + i;
          if (itemIdx == gSettingsEditor.categoryIndex) {
            oledDisplay->fillRect(0, y, 128, 10, DISPLAY_COLOR_WHITE);
            oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
          } else {
            oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          }
          
          oledDisplay->setCursor(2, y + 1);
          oledDisplay->println(modules[itemIdx]->name);
          y += lineHeight;
        }
        
        // Draw scroll indicators
        if (scrollOffset > 0) {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          oledDisplay->setCursor(120, OLED_CONTENT_START_Y);
          oledDisplay->print("\x18");  // Up arrow
        }
        if (scrollOffset + maxVisibleItems < (int)moduleCount) {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          oledDisplay->setCursor(120, OLED_CONTENT_START_Y + (maxVisibleItems - 1) * lineHeight);
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
      const int itemStartY = OLED_CONTENT_START_Y;
      const int lineHeight = 10;
      const int maxVisibleItems = OLED_CONTENT_HEIGHT / lineHeight;  // 4 items
      
      // Calculate scroll offset to keep selected item visible
      static int itemScrollOffset = 0;
      if (visibleIndex < itemScrollOffset) {
        itemScrollOffset = visibleIndex;
      } else if (visibleIndex >= itemScrollOffset + maxVisibleItems) {
        itemScrollOffset = visibleIndex - maxVisibleItems + 1;
      }
      
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
        
        if ((int)i == gSettingsEditor.itemIndex) {
          oledDisplay->fillRect(0, y - 1, 128, 10, DISPLAY_COLOR_WHITE);
          oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
        } else {
          oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        }
        
        oledDisplay->setCursor(2, y);
        // Show label and current value - truncate to prevent wrapping
        String label = entry->label ? entry->label : entry->jsonKey;

        // Truncate label if too long (max ~15 chars to leave room for value)
        if (label.length() > 15) {
          label = label.substring(0, 14); label += '~';
        }

        // Use print instead of println to prevent wrapping
        oledDisplay->print(label);
        oledDisplay->print(":");
        if (entry->type == SETTING_STRING) {
          // String value, clipped so the row never wraps (21 cols total).
          String val = *((String*)entry->valuePtr);
          const int room = 20 - (int)label.length() - 1;
          if ((int)val.length() > room && room > 1) {
            val = val.substring(0, room - 1); val += '~';
          }
          oledDisplay->print(val);
        } else {
          oledDisplay->print(getSettingCurrentValue(entry));
        }
        
        displayedCount++;
        currentVisibleIdx++;
      }
      
      // Draw scroll indicators
      if (itemScrollOffset > 0) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(120, itemStartY);
        oledDisplay->print("\x18");  // Up arrow
      }
      if (itemScrollOffset + maxVisibleItems < visibleCount) {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        oledDisplay->setCursor(120, itemStartY + (maxVisibleItems - 1) * lineHeight);
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
      oledDisplay->setCursor(0, OLED_CONTENT_START_Y);
      String label = gSettingsEditor.currentEntry->label ? gSettingsEditor.currentEntry->label : gSettingsEditor.currentEntry->jsonKey;
      oledDisplay->println(label);

      if (gSettingsEditor.editKind == SETTINGS_EDIT_PICKLIST) {
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
          if (!enumOptionAt(gSettingsEditor.currentEntry->options, idx,
                            nullptr, 0, optLabel, sizeof(optLabel))) {
            break;
          }
          if (idx == gSettingsEditor.optionIndex) {
            oledDisplay->fillRect(0, y, 128, 10, DISPLAY_COLOR_WHITE);
            oledDisplay->setTextColor(DISPLAY_COLOR_BLACK, DISPLAY_COLOR_WHITE);
          } else {
            oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
          }
          oledDisplay->setCursor(2, y + 1);
          oledDisplay->print(optLabel);
          y += lineHeight;
        }
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
        if (gSettingsEditor.optionScroll > 0) {
          oledDisplay->setCursor(120, listStartY);
          oledDisplay->print("\x18");
        }
        if (gSettingsEditor.optionScroll + maxVisible < gSettingsEditor.optionCount) {
          oledDisplay->setCursor(120, listStartY + (maxVisible - 1) * lineHeight);
          oledDisplay->print("\x19");
        }
        break;
      }

      // Draw slider
      bool isBool = (gSettingsEditor.currentEntry->type == SETTING_BOOL);
      int minVal = gSettingsEditor.currentEntry->minVal;
      int maxVal = gSettingsEditor.currentEntry->maxVal;

      drawSettingsSlider(oledDisplay, OLED_CONTENT_START_Y + 14, minVal, maxVal, gSettingsEditor.editValue, isBool);

      // Show change indicator
      if (gSettingsEditor.hasChanges) {
        oledDisplay->setCursor(0, OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 10);
        oledDisplay->print("* Modified");
      }

      break;
    }
  }
  
  // Don't call display() here - let updateOLEDDisplay() render footer and display in same frame
}

// ============================================================================
// Input Handling
// ============================================================================

bool handleSettingsEditorInput(int /*deltaX*/, int /*deltaY*/, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;

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
        gSettingsEditor.errorMessage = "Empty - not saved";
        gSettingsEditor.errorDisplayUntil = millis() + 2000;
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
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
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
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
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
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
  switch (gSettingsEditor.state) {
    case SETTINGS_CATEGORY_SELECT:
      // Enter selected category
      if (gSettingsEditor.categoryIndex < (int)moduleCount) {
        gSettingsEditor.currentModule = modules[gSettingsEditor.categoryIndex];
        gSettingsEditor.itemIndex = 0;
        
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
        gSettingsEditor.errorMessage = "Not editable";
        gSettingsEditor.errorDisplayUntil = millis() + 2000;
        break;
      }

      // Dispatch on edit kind: enum pick-list (INT or STRING with an options
      // string), keyboard (plain STRING), or the original slider (INT/BOOL).
      if (entryHasEnumOptions(gSettingsEditor.currentEntry) &&
          gSettingsEditor.currentEntry->type != SETTING_BOOL) {
        gSettingsEditor.editKind = SETTINGS_EDIT_PICKLIST;
        gSettingsEditor.optionCount = enumOptionCount(gSettingsEditor.currentEntry->options);
        gSettingsEditor.optionIndex = enumOptionIndexForCurrent(gSettingsEditor.currentEntry);
        gSettingsEditor.optionScroll = 0;
        gSettingsEditor.hasChanges = false;
        gSettingsEditor.state = SETTINGS_VALUE_EDIT;
      } else if (gSettingsEditor.currentEntry->type == SETTING_STRING) {
        const String& cur = *((String*)gSettingsEditor.currentEntry->valuePtr);
        if ((int)cur.length() > OLED_KEYBOARD_MAX_LENGTH) {
          // The keyboard buffer would silently clip the prefill and a save
          // would corrupt the value — refuse rather than truncate.
          gSettingsEditor.errorMessage = "Too long for OLED edit";
          gSettingsEditor.errorDisplayUntil = millis() + 2500;
          break;
        }
        gSettingsEditor.editKind = SETTINGS_EDIT_KEYBOARD;
        const char* label = gSettingsEditor.currentEntry->label
                                ? gSettingsEditor.currentEntry->label
                                : gSettingsEditor.currentEntry->jsonKey;
        oledKeyboardInit(label, cur.c_str(), OLED_KEYBOARD_MAX_LENGTH);
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
        if (!enumOptionAt(gSettingsEditor.currentEntry->options,
                          gSettingsEditor.optionIndex, val, sizeof(val),
                          nullptr, 0)) {
          break;
        }
        bool ok;
        if (gSettingsEditor.currentEntry->type == SETTING_STRING) {
          ok = setSettingValueStr(gSettingsEditor.currentEntry, String(val));
        } else {
          setSettingValue(gSettingsEditor.currentEntry, atoi(val));
          ok = true;
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
        gSettingsEditor.errorMessage = errorMsg;
        gSettingsEditor.errorDisplayUntil = millis() + 2000;
        break;
      }

      // Apply via command system — generates [CMD] audit line and persists
      setSettingValue(gSettingsEditor.currentEntry, gSettingsEditor.editValue);

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
  const SettingsModule** modules = getSettingsModules(moduleCount);
  
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
      gSettingsEditor.errorMessage = "";
      gSettingsEditor.errorDisplayUntil = 0;
      gSettingsEditor.editKind = SETTINGS_EDIT_SLIDER;
      gSettingsEditor.optionIndex = 0;
      gSettingsEditor.optionCount = 0;
      gSettingsEditor.optionScroll = 0;

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

// Forward declaration for registerOLEDModes
void registerOLEDModes(const OLEDModeEntry* modes, size_t count);

// Force linker to include this file - must be called from oled_display.cpp
void forceSettingsModeLink() {
  DEBUG_SYSTEMF("[SettingsMode] forceSettingsModeLink() called - file is linked");
}

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
static const OLEDModeEntry settingsModeEntry = {
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
static const OLEDModeEntry settingsOLEDModes[] = {
  settingsModeEntry
};

// Auto-register settings mode
REGISTER_OLED_MODE_MODULE(settingsOLEDModes, sizeof(settingsOLEDModes) / sizeof(settingsOLEDModes[0]), "Settings");

// ============================================================================
// Quick Settings Mode (merged from oled_quick_settings.cpp)
// ============================================================================

#if ENABLE_WIFI
#include <WiFi.h>
#endif
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
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
static int quickItemCount = 0;
static bool quickItemsInitialized = false;

static int quickSelectedItem = 0;

static char quickStatusMsg[32] = "";
static unsigned long quickStatusExpireMs = 0;

static void setQuickStatus(const char* msg, unsigned long durationMs = 2000) {
  strncpy(quickStatusMsg, msg, 31);
  quickStatusMsg[31] = '\0';
  quickStatusExpireMs = millis() + durationMs;
}

static void addQuickItem(const char* name, QuickGetStateFunc getState, QuickToggleFunc toggle) {
  if (quickItemCount < MAX_QUICK_ITEMS) {
    quickItems[quickItemCount] = { name, getState, toggle };
    quickItemCount++;
  }
}

// --- Radio power (airplane mode) ---
// The RADIO axis: whether the 2.4GHz radio is powered up at all (ON whenever
// WiFi is connecting/connected OR ESP-NOW holds it). Separate from the WiFi
// CONNECTION row below. Toggling off powers the whole radio down (ESP-NOW too);
// runtime only — the radio returns to its configured state on the next boot.
#if ENABLE_WIFI
static bool getQuickRadioState() {
  extern bool wifiRadioOn();
  return wifiRadioOn();
}
static void toggleQuickRadio() {
  extern bool wifiRadioOn();
  if (wifiRadioOn()) {
    setQuickStatus("Radio OFF");
    executeOLEDCommand("radiopower off");
  } else {
    setQuickStatus("Radio ON");
    executeOLEDCommand("radiopower on");
  }
}
#endif

// --- WiFi ---
#if ENABLE_WIFI
// State reflects the CONNECTION, not the radio mode: ESP-NOW holds the radio in
// WIFI_AP_STA (mode never NULL), so keying on getMode() left this stuck ON.
static bool getQuickWiFiState() {
  return WiFi.isConnected();
}
static void toggleQuickWiFi() {
  extern bool wifiRadioOn();
  // Can't connect a network with the radio powered down — mirror the HTTP row's
  // "Need WiFi first!" refusal instead of firing openwifi against a dead radio.
  if (!wifiRadioOn()) {
    setQuickStatus("Radio off!");
    return;
  }
  if (WiFi.isConnected()) {
    setQuickStatus("WiFi OFF");
    executeOLEDCommand("closewifi");
  } else {
    setQuickStatus("WiFi ON");
    executeOLEDCommand("openwifi --best");
  }
}
#endif

// --- Bluetooth ---
#if ENABLE_BLUETOOTH
static bool getQuickBluetoothState() {
  extern bool isBLERunning();
  return isBLERunning();
}
static void bluetoothToggleConfirmedQuick(void* userData) {
  (void)userData;
  extern bool isBLERunning();
  if (isBLERunning()) {
    setQuickStatus("Bluetooth OFF");
    executeOLEDCommand("closeble");
  } else {
    setQuickStatus("Bluetooth ON");
    executeOLEDCommand("openble");
  }
}
static void toggleQuickBluetooth() {
  extern bool isBLERunning();
  if (isBLERunning()) {
    oledConfirmRequest("Stop Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr, false);
  } else {
    oledConfirmRequest("Start Bluetooth?", nullptr, bluetoothToggleConfirmedQuick, nullptr);
  }
}
#endif

// --- HTTP Server ---
#if ENABLE_HTTP_SERVER
static bool getQuickHTTPState() {
  return (server != nullptr);
}
static void httpToggleConfirmedQuick(void* userData) {
  (void)userData;
  if (server != nullptr) {
    setQuickStatus("HTTP OFF");
    executeOLEDCommand("closehttp");
  } else {
    if (!WiFi.isConnected()) {
      setQuickStatus("Need WiFi first!");
      return;
    }
    setQuickStatus("HTTP ON");
    executeOLEDCommand("openhttp");
  }
}
static void toggleQuickHTTP() {
  if (server != nullptr) {
    oledConfirmRequest("Stop HTTP?", nullptr, httpToggleConfirmedQuick, nullptr, false);
  } else {
    if (!WiFi.isConnected()) {
      setQuickStatus("Need WiFi first!");
      return;
    }
    oledConfirmRequest("Start HTTP?", nullptr, httpToggleConfirmedQuick, nullptr);
  }
}
#endif

static void initQuickItems() {
  if (quickItemsInitialized) return;
  quickItemsInitialized = true;
  quickItemCount = 0;
#if ENABLE_WIFI
  addQuickItem("Radio", getQuickRadioState, toggleQuickRadio);  // radio power (airplane mode) — above WiFi
  addQuickItem("WiFi", getQuickWiFiState, toggleQuickWiFi);      // network connection
#endif
#if ENABLE_BLUETOOTH
  addQuickItem("Bluetooth", getQuickBluetoothState, toggleQuickBluetooth);
#endif
#if ENABLE_HTTP_SERVER
  addQuickItem("HTTP Server", getQuickHTTPState, toggleQuickHTTP);
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

  if (quickItemCount == 0) {
    oledDisplay->setCursor(4, OLED_CONTENT_START_Y + 14);
    oledDisplay->print("No toggles available");
  } else {
    // Layout adapts to the row count so it always fits the 43px content area.
    // <=3 toggles keep the original +5/step-13/12px-highlight look (last
    // highlight Y=40..51, 1px above the footer boundary). 4+ toggles (Radio +
    // WiFi + Bluetooth + HTTP) shrink the step/highlight to stay on the 64px
    // panel: step = (CONTENT_HEIGHT-3)/count (e.g. 10 for 4 rows).
    const bool tight = (quickItemCount > 3);
    const int stepPx  = tight ? ((OLED_CONTENT_HEIGHT - 3) / quickItemCount) : 13;
    const int hiliteH = tight ? (stepPx - 1) : 12;
    int yPos = OLED_CONTENT_START_Y + (tight ? 3 : 5);
    for (int i = 0; i < quickItemCount; i++) {
      bool isSelected = (i == quickSelectedItem);
      bool isEnabled = quickItems[i].getState ? quickItems[i].getState() : false;

      if (isSelected) {
        oledDisplay->fillRect(0, yPos - 2, 128, hiliteH, DISPLAY_COLOR_WHITE);
        oledDisplay->setTextColor(DISPLAY_COLOR_BLACK);
      } else {
        oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
      }

      oledDisplay->setCursor(4, yPos);
      oledDisplay->print(quickItems[i].name);

      oledDisplay->setCursor(90, yPos);
      oledDisplay->print(isEnabled ? "[ON]" : "[OFF]");

      yPos += stepPx;
    }
  }
  
  if (quickStatusMsg[0] != '\0' && millis() < quickStatusExpireMs) {
    // Draw status toast at bottom of content area (above footer)
    oledDisplay->setTextColor(DISPLAY_COLOR_WHITE);
    oledDisplay->setCursor(0, OLED_CONTENT_HEIGHT - 10);
    oledDisplay->print(quickStatusMsg);
  } else if (quickStatusMsg[0] != '\0') {
    quickStatusMsg[0] = '\0';
  }
}

// Input handler
bool quickSettingsInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  if (oledGuestBlocksMutate()) return true;
  initQuickItems();
  if (quickItemCount == 0) {
    if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
      requestOLEDMode(popOLEDMode(), "quicksettings.back.empty", false, /*isBackNav=*/true);
      return true;
    }
    return false;
  }
  
  bool handled = false;
  
  if (gNavEvents.up) {
    quickSelectedItem = (quickSelectedItem - 1 + quickItemCount) % quickItemCount;
    handled = true;
  } else if (gNavEvents.down) {
    quickSelectedItem = (quickSelectedItem + 1) % quickItemCount;
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A)) {
    if (quickSelectedItem >= 0 && quickSelectedItem < quickItemCount && quickItems[quickSelectedItem].toggle) {
      quickItems[quickSelectedItem].toggle();
    }
    handled = true;
  }
  
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    requestOLEDMode(popOLEDMode(), "quicksettings.back", false, /*isBackNav=*/true);
    handled = true;
  }
  
  return handled;
}

// Note: Quick settings mode is registered directly in oled_display.cpp
// to ensure it's always linked and available (accessed via SELECT button)

#endif // ENABLE_OLED_DISPLAY
