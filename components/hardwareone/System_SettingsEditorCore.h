#ifndef SYSTEM_SETTINGSEDITORCORE_H
#define SYSTEM_SETTINGSEDITORCORE_H

// =============================================================================
// Settings-editor core — display-independent helpers
// =============================================================================
// The logic an interactive settings editor needs that is NOT tied to any one
// output surface: entry visibility, the editability predicate, the width-
// correct current-value read, the enum-"options" string parser, and command
// resolution. Previously all of this lived file-static inside
// OLED_SettingsEditor.cpp — which is compiled ONLY under ENABLE_OLED_DISPLAY,
// so the G2 lens (present on XIAO builds that have no OLED) could not reach it.
//
// This TU compiles unconditionally on every board. The OLED editor keeps thin
// wrappers over these functions so its call sites and behaviour are unchanged;
// the G2 interactive settings page calls them directly.
//
// See docs/G2_INTERACTIVE_SETTINGS_PLAN.md.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

#include "System_Settings.h"   // SettingEntry, SettingType

// -----------------------------------------------------------------------------
// Editability type policy
// -----------------------------------------------------------------------------
// A surface declares which SettingType kinds it can actually edit as a bitmask
// (bit N = (1 << SETTING_*)). The OLED editor historically only edits
// INT/BOOL/STRING because its value path is a single int + slider. The G2 lens
// enters every scalar through the character keyboard, so it can also edit the
// explicit-width integer types and floats.
enum SettingsEditTypeBit : uint32_t {
  SETTINGS_EDIT_BIT_INT    = 1u << SETTING_INT,
  SETTINGS_EDIT_BIT_FLOAT  = 1u << SETTING_FLOAT,
  SETTINGS_EDIT_BIT_BOOL   = 1u << SETTING_BOOL,
  SETTINGS_EDIT_BIT_STRING = 1u << SETTING_STRING,
  SETTINGS_EDIT_BIT_U8     = 1u << SETTING_U8,
  SETTINGS_EDIT_BIT_U16    = 1u << SETTING_U16,
  SETTINGS_EDIT_BIT_U32    = 1u << SETTING_U32,
};

// OLED's historical scope (int editValue path): INT / BOOL / STRING only.
#define SETTINGS_EDIT_MASK_OLED \
  (SETTINGS_EDIT_BIT_INT | SETTINGS_EDIT_BIT_BOOL | SETTINGS_EDIT_BIT_STRING)

// Every editable scalar type — the G2 keyboard can type any of them.
#define SETTINGS_EDIT_MASK_ALL                                       \
  (SETTINGS_EDIT_BIT_INT | SETTINGS_EDIT_BIT_FLOAT |                 \
   SETTINGS_EDIT_BIT_BOOL | SETTINGS_EDIT_BIT_STRING |              \
   SETTINGS_EDIT_BIT_U8 | SETTINGS_EDIT_BIT_U16 | SETTINGS_EDIT_BIT_U32)

// -----------------------------------------------------------------------------
// Visibility + editability
// -----------------------------------------------------------------------------

// Some entries are conditionally hidden (e.g. an I2C-clock setting for a sensor
// that isn't compiled in or isn't connected). Returns false to hide the entry.
bool settingsEditorIsVisible(const SettingEntry* entry);

// The single editability predicate: visible, not readOnly, not isSecret, and a
// type the surface's `typeMask` allows. Secrets stay excluded on purpose — no
// on-lens/on-panel surface has a masked-input mode, so prefilling one would
// paint the secret on screen.
bool settingsEditorIsEditable(const SettingEntry* entry, uint32_t typeMask);

// -----------------------------------------------------------------------------
// Width-correct current-value read (INT/U8/U16/U32/BOOL → int)
// -----------------------------------------------------------------------------
// Reading 4 bytes through a uint8_t pointer (the old `*((int*)valuePtr)`
// formulation) caused the 2026-05-18 heap-corruption crash; dispatch on the
// declared width. FLOAT/STRING return 0 (callers read those directly).
int settingsEditorCurrentValue(const SettingEntry* entry);

// -----------------------------------------------------------------------------
// Enum "options" string parser
// -----------------------------------------------------------------------------
// Format (System_Settings.h): a comma list of tokens, each "value|label"
// (legacy ':' also accepted) or a bare token that is both value and label. A
// "bitmask:" prefix marks a checkbox-grid hint for the web renderer — NOT an
// enum — and is rejected here.

// True if the entry has a usable enum options string (present and not bitmask).
bool settingsEditorHasEnumOptions(const SettingEntry* entry);

// Number of comma-separated options in the string (0 for null/empty).
int settingsEditorEnumCount(const char* options);

// Copy option `idx`'s value and/or label (either dest may be null to skip).
// Returns false when idx is out of range.
bool settingsEditorEnumAt(const char* options, int idx,
                          char* valueOut, size_t valueCap,
                          char* labelOut, size_t labelCap);

// Index of the option whose VALUE matches the entry's current value (for
// pick-list prefill). Returns 0 when nothing matches (value set off-list).
int settingsEditorEnumIndexForCurrent(const SettingEntry* entry);

// -----------------------------------------------------------------------------
// Command resolution
// -----------------------------------------------------------------------------

// The CLI command name that mutates this setting: cmdKey if set, else jsonKey.
// (Command matching is case-insensitive, so a camelCase jsonKey works directly
// as the command name.) Returns nullptr only for a null/keyless entry.
const char* settingsEditorCommandName(const SettingEntry* entry);

// True if settingsEditorCommandName resolves to a command that is actually
// registered. Per-setting command coverage is hand-maintained (no compile-time
// assert), so an editor should refuse entries whose command is missing rather
// than dispatch an "Unknown command" that would otherwise read as success.
bool settingsEditorHasCommand(const SettingEntry* entry);

#endif  // SYSTEM_SETTINGSEDITORCORE_H
