# Command Registration Patterns - Research Findings

## Current State Analysis

Analyzed **21 command modules** across the codebase. Found **3 distinct patterns** with varying levels of consistency.

---

## Pattern Analysis

### Pattern 1: Standard Modular Pattern (Most Common - 15/21 modules)
**Used by:** IMU, Thermal, ToF, Gamepad, APDS, GPS, FM Radio, Servo, WiFi, I2C, Filesystem, NeoPixel, Settings

**Structure:**
```cpp
// ============================================================================
// <Module> Command Registry
// ============================================================================

const CommandEntry <module>Commands[] = {
  // Optional category comments
  { "cmd1", "Description", false, handler },
  { "cmd2", "Description with params", true, handler, "Usage: cmd2 <arg>" }
};

const size_t <module>CommandsCount = sizeof(<module>Commands) / sizeof(<module>Commands[0]);

// ============================================================================
// Command Registration
// ============================================================================
static CommandModuleRegistrar _<module>_registrar(<module>Commands, <module>CommandsCount, "<module>");
```

**Examples:**

**i2csensor-bno055.cpp (IMU) - EXCELLENT:**
```cpp
// IMU Command Registry (Sensor-Specific)
// ============================================================================
const CommandEntry imuCommands[] = {
  // Start/Stop
  { "imustart", "Start BNO055 IMU sensor.", false, cmd_imustart },
  { "imustop", "Stop BNO055 IMU sensor.", false, cmd_imustop },
  
  // Information
  { "imu", "Read IMU sensor data.", false, cmd_imu },
  { "imuactions", "Show IMU action detection state.", false, cmd_imuactions },
  
  // UI Settings (client-side visualization)
  { "imupollingms", "Set IMU UI polling interval (50..2000ms).", true, cmd_imupollingms, "Usage: imupollingms <50..2000>" },
  // ... more commands
};

const size_t imuCommandsCount = sizeof(imuCommands) / sizeof(imuCommands[0]);

// ============================================================================
// Command Registration (Sensor-Specific)
// ============================================================================
// Direct static registration to avoid macro issues
static CommandModuleRegistrar _imu_cmd_registrar(imuCommands, imuCommandsCount, "imu");
```
**Strengths:** Clear sections, grouped by category, consistent formatting, helpful comments

**i2csensor-apds9960.cpp (APDS) - GOOD:**
```cpp
// APDS Command Registry
// ============================================================================

const CommandEntry apdsCommands[] = {
  // Primary commands (queue-based startup, consistent with other sensors)
  { "apdsstart", "Start APDS9960 sensor.", false, cmd_apdsstart },
  { "apdsstop", "Stop APDS9960 sensor.", false, cmd_apdsstop },
  // ... more commands
};

const size_t apdsCommandsCount = sizeof(apdsCommands) / sizeof(apdsCommands[0]);
static CommandModuleRegistrar _apds_registrar(apdsCommands, apdsCommandsCount, "apds");
```
**Strengths:** Clean, minimal, consistent

**System_WiFi.cpp - MINIMAL:**
```cpp
// WiFi Command Registry
// ============================================================================

const CommandEntry wifiCommands[] = {
  { "wifiinfo", "Show current WiFi connection info.", false, cmd_wifiinfo },
  { "wifilist", "List saved WiFi networks.", false, cmd_wifilist },
  // ... more commands
};

const size_t wifiCommandsCount = sizeof(wifiCommands) / sizeof(wifiCommands[0]);
static CommandModuleRegistrar _wifi_registrar(wifiCommands, wifiCommandsCount, "wifi");
```
**Strengths:** Simple, no clutter

---

### Pattern 2: Stub Pattern (Disabled Modules - 8/21 modules)
**Used by:** Sensor stubs when features are disabled

**Structure:**
```cpp
const struct CommandEntry <module>Commands[] = {};
const size_t <module>CommandsCount = 0;
```

**Example from sensor_stubs_minimal.cpp:**
```cpp
#if !ENABLE_THERMAL_SENSOR
const struct CommandEntry thermalCommands[] = {};
const size_t thermalCommandsCount = 0;
#endif
```

**Purpose:** Allows compilation when features are disabled
**Status:** Correct and necessary - no changes needed

---

### Pattern 3: Inline Registration (Rare - 1/21 modules)
**Used by:** System_Debug_Commands.cpp

**Structure:**
```cpp
static const CommandEntry logLevelCommands[] = {
  { "loglevel", "Set log level (error|warn|info|debug)", true, cmd_loglevel }
};

static CommandModuleRegistrar _loglevel_registrar(logLevelCommands, 
                                                   sizeof(logLevelCommands)/sizeof(logLevelCommands[0]), 
                                                   "debug");
```

**Observation:** Uses `static const` and inline size calculation
**Status:** Works but inconsistent with other modules

---

## Inconsistencies Found

### 1. Header Comment Variations
```cpp
// Variation A (Most common):
// ============================================================================
// IMU Command Registry (Sensor-Specific)
// ============================================================================

// Variation B:
// APDS Command Registry
// ============================================================================

// Variation C:
// WiFi Command Registry
// ============================================================================

// Variation D (minimal):
// Command Registry
// ============================================================================
```

### 2. Registrar Variable Naming
```cpp
// Pattern A (most common):
static CommandModuleRegistrar _imu_cmd_registrar(...);

// Pattern B (simpler):
static CommandModuleRegistrar _apds_registrar(...);

// Pattern C (inline):
CommandModuleRegistrar _thermal_cmd_registrar(...);  // Missing 'static'
```

### 3. Category Comments
```cpp
// Some modules use category grouping:
const CommandEntry imuCommands[] = {
  // Start/Stop
  { "imustart", ... },
  { "imustop", ... },
  
  // Information
  { "imu", ... },
  // ...
};

// Others don't:
const CommandEntry wifiCommands[] = {
  { "wifiinfo", ... },
  { "wifilist", ... },
  // No grouping
};
```

### 4. Registration Section Comments
```cpp
// Variation A (detailed):
// ============================================================================
// Command Registration (Sensor-Specific)
// ============================================================================
// Direct static registration to avoid macro issues

// Variation B (minimal):
// No comment at all, just the registrar line
```

---

## Recommendations

### Proposed Standard Pattern

```cpp
// ============================================================================
// <Module Name> Command Registry
// ============================================================================

const CommandEntry <module>Commands[] = {
  // Category: <Group Name> (optional, use when >5 commands)
  { "cmd1", "Description", false, handler },
  { "cmd2", "Description", true, handler, "Usage: ..." },
  
  // Category: <Another Group> (optional)
  { "cmd3", "Description", false, handler }
};

const size_t <module>CommandsCount = sizeof(<module>Commands) / sizeof(<module>Commands[0]);

// ============================================================================
// Command Registration
// ============================================================================
static CommandModuleRegistrar _<module>_registrar(<module>Commands, <module>CommandsCount, "<module>");
```

### Rules for Consistency

1. **Header Comment:**
   - Format: `// ============================================================================`
   - Text: `// <Module Name> Command Registry`
   - Closing: `// ============================================================================`

2. **Array Declaration:**
   - Format: `const CommandEntry <module>Commands[] = {`
   - Use category comments for modules with >5 commands
   - Group related commands together

3. **Size Calculation:**
   - Always use: `const size_t <module>CommandsCount = sizeof(...) / sizeof(...[0]);`
   - Place immediately after array

4. **Registration Section:**
   - Header: `// ============================================================================`
   - Text: `// Command Registration`
   - Closing: `// ============================================================================`
   - Registrar: `static CommandModuleRegistrar _<module>_registrar(...);`

5. **Registrar Naming:**
   - Format: `_<module>_registrar` (simple, no `_cmd` suffix)
   - Always use `static` keyword

6. **Category Comments:**
   - Use for modules with >5 commands
   - Format: `// Category: <Name>` or just `// <Name>`
   - Examples: `// Start/Stop`, `// Information`, `// Settings`

---

## Files Requiring Updates

### High Priority (Inconsistent Patterns)
1. **System_Debug_Commands.cpp** - Uses inline size calculation
2. **i2csensor-mlx90640.cpp** - Missing `static` on registrar

### Medium Priority (Missing Category Comments)
3. **System_WiFi.cpp** - 13 commands, no grouping
4. **System_I2C.cpp** - 11 commands, no grouping
5. **i2csensor-rda5807.cpp** - 9 commands, no grouping

### Low Priority (Minor Header Variations)
6. **i2csensor-apds9960.cpp** - Header comment variation
7. **i2csensor-vl53l4cx.cpp** - Header comment variation
8. **i2csensor-seesaw.cpp** - Header comment variation
9. **i2csensor-pa1010d.cpp** - Header comment variation
10. **i2csensor-pca9685.cpp** - Header comment variation
11. **neopixel_led.cpp** - Header comment variation
12. **filesystem.cpp** - Header comment variation
13. **settings.cpp** - Header comment variation

---

## Impact Assessment

**Files to Modify:** 13 files (out of 21 modules)
**Changes Per File:** 5-10 lines (comments and formatting)
**Risk:** VERY LOW (formatting only, no code changes)
**Time:** 30-45 minutes
**Benefit:** 
- Easier to navigate codebase
- Consistent structure for adding new commands
- Better code organization

---

## Example Transformation

### Before (System_WiFi.cpp):
```cpp
// WiFi Command Registry
// ============================================================================

const CommandEntry wifiCommands[] = {
  { "wifiinfo", "Show current WiFi connection info.", false, cmd_wifiinfo },
  { "wifilist", "List saved WiFi networks.", false, cmd_wifilist },
  { "wifiadd", "Add/overwrite a WiFi network.", true, cmd_wifiadd, "Usage: wifiadd <ssid> <pass> [priority] [hidden0|1]" },
  { "wifirm", "Remove a WiFi network.", true, cmd_wifirm, "Usage: wifirm <ssid>" },
  { "wifipromote", "Promote a WiFi network to top priority.", true, cmd_wifipromote, "Usage: wifipromote <ssid>" },
  { "wificonnect", "Connect to WiFi (auto-select or specify SSID).", false, cmd_wificonnect, "Usage: wificonnect [ssid]" },
  { "wifidisconnect", "Disconnect from WiFi.", false, cmd_wifidisconnect },
  { "wifiscan", "Scan for available WiFi networks.", false, cmd_wifiscan },
  { "wifigettxpower", "Get WiFi TX power.", false, cmd_wifitxpower },
  { "ntpsync", "Sync time with NTP server.", false, cmd_ntpsync },
  { "httpstart", "Start HTTP server.", false, cmd_httpstart },
  { "httpstop", "Stop HTTP server.", false, cmd_httpstop },
  { "httpstatus", "Show HTTP server status.", false, cmd_httpstatus }
};

const size_t wifiCommandsCount = sizeof(wifiCommands) / sizeof(wifiCommands[0]);
static CommandModuleRegistrar _wifi_registrar(wifiCommands, wifiCommandsCount, "wifi");
```

### After (System_WiFi.cpp):
```cpp
// ============================================================================
// WiFi Command Registry
// ============================================================================

const CommandEntry wifiCommands[] = {
  // Network Management
  { "wifiinfo", "Show current WiFi connection info.", false, cmd_wifiinfo },
  { "wifilist", "List saved WiFi networks.", false, cmd_wifilist },
  { "wifiadd", "Add/overwrite a WiFi network.", true, cmd_wifiadd, "Usage: wifiadd <ssid> <pass> [priority] [hidden0|1]" },
  { "wifirm", "Remove a WiFi network.", true, cmd_wifirm, "Usage: wifirm <ssid>" },
  { "wifipromote", "Promote a WiFi network to top priority.", true, cmd_wifipromote, "Usage: wifipromote <ssid>" },
  
  // Connection Control
  { "wificonnect", "Connect to WiFi (auto-select or specify SSID).", false, cmd_wificonnect, "Usage: wificonnect [ssid]" },
  { "wifidisconnect", "Disconnect from WiFi.", false, cmd_wifidisconnect },
  { "wifiscan", "Scan for available WiFi networks.", false, cmd_wifiscan },
  { "wifigettxpower", "Get WiFi TX power.", false, cmd_wifitxpower },
  
  // Network Services
  { "ntpsync", "Sync time with NTP server.", false, cmd_ntpsync },
  { "httpstart", "Start HTTP server.", false, cmd_httpstart },
  { "httpstop", "Stop HTTP server.", false, cmd_httpstop },
  { "httpstatus", "Show HTTP server status.", false, cmd_httpstatus }
};

const size_t wifiCommandsCount = sizeof(wifiCommands) / sizeof(wifiCommands[0]);

// ============================================================================
// Command Registration
// ============================================================================
static CommandModuleRegistrar _wifi_registrar(wifiCommands, wifiCommandsCount, "wifi");
```

---

## Conclusion

**Current State:** Generally consistent with minor variations
**Recommendation:** Standardize headers and add category comments to large modules
**Priority:** LOW-MEDIUM (nice to have, not critical)
**Effort:** Minimal (formatting only)
