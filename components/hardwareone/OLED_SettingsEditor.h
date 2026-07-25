#ifndef OLED_SETTINGSEDITOR_H
#define OLED_SETTINGSEDITOR_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Arduino.h>

#include "System_Settings.h"

// Settings editor state machine
enum SettingsEditorState {
  SETTINGS_CATEGORY_SELECT,  // Selecting a settings module/category
  SETTINGS_ITEM_SELECT,       // Selecting a setting within the category
  SETTINGS_VALUE_EDIT         // Editing the value (slider / keyboard / pick-list)
};

// How the VALUE_EDIT level edits the current entry. INT/BOOL without options
// keep the original slider; entries with an enum-style options string
// ("0|I2C1,1|I2C2" or plain "logo,status,...") get a pick-list; STRING
// entries without options get the on-screen keyboard prefilled with the
// current value. "bitmask:"-prefixed options are NOT enums (checkbox-grid
// hint for the web) and fall through to the slider path.
enum SettingsEditKind {
  SETTINGS_EDIT_SLIDER = 0,
  SETTINGS_EDIT_KEYBOARD,
  SETTINGS_EDIT_PICKLIST
};

// Settings editor context
struct SettingsEditorContext {
  SettingsEditorState state;
  int categoryIndex;          // Current category (module) index
  int itemIndex;              // Current setting index within category
  int editValue;              // Current value being edited (for int/bool)
  bool hasChanges;            // Whether current edit has unsaved changes
  const SettingsModule* currentModule;
  const SettingEntry* currentEntry;
  String errorMessage;        // Error message to display
  unsigned long errorDisplayUntil;  // Timestamp when error should clear
  SettingsEditKind editKind;  // Active VALUE_EDIT variant
  int optionIndex;            // Pick-list cursor (index into parsed options)
  int optionCount;            // Parsed option count for the current entry
  int optionScroll;           // Pick-list scroll offset (kept in the context —
                              // the static-local scroll offsets elsewhere in the
                              // editor persist across module switches, a quirk
                              // this field deliberately avoids inheriting)
};

// Global settings editor context
extern SettingsEditorContext gSettingsEditor;

// Initialize settings editor
void initSettingsEditor();

// Reset to category selection
void resetSettingsEditor();

// Display settings editor (called from OLED display loop)
void displaySettingsEditor();

// Handle input for settings editor
// Returns true if input was handled
bool handleSettingsEditorInput(int deltaX, int deltaY, uint32_t newlyPressed);

// Navigation functions
void settingsEditorUp();
void settingsEditorDown();
void settingsEditorSelect();
void settingsEditorBack();

// Open settings editor directly to a specific module by name
// Returns true if module was found and editor was opened
bool openSettingsEditorForModule(const char* moduleName);

#endif // ENABLE_OLED_DISPLAY

#endif // OLED_SETTINGSEDITOR_H
