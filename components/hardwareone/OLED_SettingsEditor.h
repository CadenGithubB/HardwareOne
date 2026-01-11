#ifndef OLED_SETTINGS_EDITOR_H
#define OLED_SETTINGS_EDITOR_H

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

#include <Arduino.h>

#include "System_Settings.h"

// Settings editor state machine
enum SettingsEditorState {
  SETTINGS_CATEGORY_SELECT,  // Selecting a settings module/category
  SETTINGS_ITEM_SELECT,       // Selecting a setting within the category
  SETTINGS_VALUE_EDIT         // Editing the value with slider
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

#endif // OLED_SETTINGS_EDITOR_H
