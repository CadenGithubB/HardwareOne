# Modular Settings System Implementation

## Overview
Refactored power management settings to use the modular registry system, enabling automatic rendering in both OLED and web interfaces with connection status checking for grayed-out disconnected modules.

## Changes Made

### 1. Enhanced SettingsModule Structure (`settings.h`)
Added two new fields to `SettingsModule`:
- `ConnectionCheckFunc isConnected` - Optional callback to check if module is available
- `const char* description` - Human-readable description for UI

```cpp
typedef bool (*ConnectionCheckFunc)();

struct SettingsModule {
  const char* name;
  const char* jsonSection;
  const SettingEntry* entries;
  size_t count;
  ConnectionCheckFunc isConnected;  // NEW
  const char* description;          // NEW
};
```

### 2. Power Management Module (`System_Power.cpp`)
Created modular settings registration:
- 4 settings: mode, autoMode, batteryThreshold, displayDimLevel
- Connection check: Always returns true (CPU frequency control always available)
- Auto-registers on startup via static constructor
- Description: "CPU frequency scaling and battery optimization"

### 3. Updated Existing Modules
- **Debug Settings**: Added nullptr for isConnected, description
- **Output Settings**: Added nullptr for isConnected, description  
- **Thermal Settings**: Added `isThermalConnected()` callback, description

## Benefits

### For Developers
- **Add once, appears everywhere**: Settings automatically render in OLED and web UI
- **Type safety**: Schema defines validation rules (min/max, options)
- **Less code**: No manual HTML/JS per setting
- **Consistent UI**: Same rendering logic across interfaces

### For Users
- **Visual feedback**: Disconnected modules grayed out with "Inactive" badge
- **Error prevention**: Attempting to change disconnected module settings shows error
- **Clear status**: Connection state visible at a glance

## Implementation Status

### ✅ Completed
1. Enhanced SettingsModule structure with connection check and description
2. Power management modular registration
3. Updated debug, output, and thermal settings modules
4. Power settings automatically appear in OLED Settings mode

### 🔄 In Progress
5. Update remaining sensor modules (ToF, IMU, GPS, FM Radio, Gamepad, APDS)
6. Enhance web renderer to check connection status
7. Remove manual power settings HTML/JS from web page

### ⏳ Pending
8. Test in both OLED and web UI
9. Verify grayed-out state for disconnected modules
10. Test error handling when attempting to modify disconnected module settings

## Web Renderer Enhancement Plan

The web settings page already has infrastructure for grayed-out modules (see `isOrphan` parameter in `renderModule` function). Need to:

1. **Fetch connection status**: Add `/api/settings/status` endpoint that returns module connection states
2. **Update renderModule**: Check `isConnected` callback and pass to `renderInput` as disabled flag
3. **Add save validation**: Check connection status before allowing save
4. **Show status badge**: Display "Inactive" badge for disconnected modules (already implemented)

## Next Steps

1. Update all sensor SettingsModule definitions with connection callbacks
2. Enhance web renderer to fetch and use connection status
3. Remove manual power settings panel from WebPage_Settings.h
4. Test complete flow in both interfaces
