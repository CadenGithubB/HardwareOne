# Command Parser Unification Plan

## Overview
Unify fragmented command argument parsing across ~200+ command handlers into a centralized `CommandArgs` utility class.

## Current State Analysis

### Fragmentation Identified
1. **Manual parsing in ~200+ commands** across:
   - `System_ESPNow.cpp`: ~50 commands with repetitive `indexOf(' ')` + `substring()` chains
   - `System_User.cpp`: ~15 commands with identical patterns
   - `System_WiFi.cpp`: ~10 commands each reimplementing parsing
   - `i2csensor-pca9685.cpp`: Nested space-finding for servo commands
   - `System_Automation.cpp`: Custom `getVal()` lambda for key=value parsing

2. **Inconsistent patterns**:
   - Some use `indexOf(' ')` repeatedly with `s1`, `s2`, `s3` tracking
   - Some use `lastIndexOf(' ')` for optional trailing args
   - No centralized validation helpers
   - Each command manually checks `if (sp1 < 0)` for missing args
   - Repeated usage strings in every command

3. **Code duplication estimate**: ~500-800 lines of repetitive parsing logic

## Implementation Plan

### Phase 1: Core CommandArgs Class (Day 1)

**File**: `System_Command.h` + `System_Command.cpp`

```cpp
class CommandArgs {
public:
    // Constructor - parses input string into arguments
    CommandArgs(const String& input);
    
    // Core positional argument access
    String getArg(int index) const;              // Get nth space-separated arg (0-indexed)
    int getArgCount() const;                     // Total argument count
    bool hasMinArgs(int count) const;            // Validation: check minimum args
    
    // Typed extraction with defaults
    int getInt(int index, int defaultVal = 0) const;
    bool getBool(int index, bool defaultVal = false) const;
    
    // Specialized parsing
    bool getMac(int index, uint8_t* mac) const;  // Parse MAC address (AA:BB:CC:DD:EE:FF)
    
    // Remaining text after N args (for commands with freeform text at end)
    String getRemainingAfter(int argIndex) const;
    
    // Key=value parsing (for automation-style commands)
    String getValue(const String& key) const;
    bool hasKey(const String& key) const;
    
    // Raw access
    const String& raw() const { return original; }
    
private:
    String original;                             // Original input string
    std::vector<String> args;                    // Parsed positional arguments
    void parsePositional();                      // Split by spaces
};
```

**Implementation details**:
- `parsePositional()`: Split input by spaces, handle quoted strings
- `getMac()`: Use existing `parseMacAddress()` helper
- `getBool()`: Accept "1", "0", "true", "false", "on", "off"
- `getValue()`: Parse `key="value"` or `key=value` patterns

### Phase 2: Helper Functions (Day 1)

Add to `System_Command.cpp`:

```cpp
// Auto-generate usage string from command signature
String generateUsage(const char* cmdName, const char* signature);

// Example: generateUsage("espnowpair", "<mac> <name>")
// Returns: "Usage: espnowpair <mac> <name>"
```

### Phase 3: Migration Strategy

#### Priority 1: ESP-NOW Commands (~50 commands, highest ROI)
**File**: `System_ESPNow.cpp`

**Before** (example from `cmd_espnow_pair`):
```cpp
String args = argsInput;
args.trim();
int firstSpace = args.indexOf(' ');
if (firstSpace < 0) return "Usage: espnow pair <mac> <name>";
String macStr = args.substring(0, firstSpace);
String name = args.substring(firstSpace + 1);
macStr.trim();
name.trim();
if (macStr.length() == 0 || name.length() == 0) {
    return "Usage: espnow pair <mac> <name>";
}
uint8_t mac[6];
if (!parseMacAddress(macStr, mac)) {
    return "Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
}
```

**After**:
```cpp
CommandArgs args(argsInput);
if (!args.hasMinArgs(2)) return "Usage: espnowpair <mac> <name>";

uint8_t mac[6];
if (!args.getMac(0, mac)) {
    return "Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
}
String name = args.getArg(1);
```

**Commands to migrate** (in order of complexity):
1. Simple 2-arg commands: `espnowpair`, `espnowunpair`, `espnowsend`
2. Multi-arg commands: `espnowremote` (4 args), `espnowbrowse` (3-4 args)
3. Complex commands: `espnowroomcmd`, `espnowtagcmd` (nested parsing)

#### Priority 2: User Management Commands (~15 commands)
**File**: `System_User.cpp`

**Commands**: `useradd`, `userchangepassword`, `userresetpassword`, `usersync`, etc.

**Pattern**: Most use 2-4 positional args with optional trailing boolean

#### Priority 3: WiFi Commands (~10 commands)
**File**: `System_WiFi.cpp`

**Commands**: `wifiadd`, `wifirm`, `wifipromote`

**Pattern**: Mix of positional and optional args

#### Priority 4: Automation Commands (~8 commands)
**File**: `System_Automation.cpp`

**Commands**: `automationadd`, `automationedit`

**Pattern**: Key=value parsing (needs `getValue()` method)

#### Priority 5: Sensor/Peripheral Commands (~20 commands)
**Files**: `i2csensor-pca9685.cpp`, `i2csensor-rda5807.cpp`, etc.

**Commands**: `servo`, `servoprofile`, `pwm`, `fmtune`, etc.

### Phase 4: Testing & Validation

**Test cases to create**:
1. Positional argument extraction
2. MAC address parsing
3. Integer/boolean parsing with defaults
4. Quoted string handling
5. Key=value parsing
6. Edge cases: empty args, trailing spaces, missing args

**Validation approach**:
- Build test after each migration batch
- Functional test key commands via CLI
- Verify error messages are consistent

## Migration Checklist

### Phase 1: Foundation
- [x] Create `CommandArgs` class in `System_Command.h`
- [x] Implement core methods in `System_Command.cpp`
- [x] Add `argMac()` helper using existing `parseMacAddress()`
- [x] Add `argInt()`, `argBool()` with validation
- [x] Add `value()`/`hasKey()` for key=value parsing

### Phase 2: ESP-NOW Migration
- [x] Batch 1 - simple commands: pair, unpair, broadcast, send, sendfile, mode, setname, stationary, hbmode, meshrole, meshttl
- [x] Batch 2 - multi-arg commands: roomcmd, tagcmd, remote, browse, fetch, worker, meshmaster, meshbackup
- [x] Batch 3 - extras: pairsecure, bigsend, bond_stream, buffers

### Phase 3: User & WiFi Migration
- [x] User commands (8): changepassword, resetpassword, add, session_revoke, ban, banuser, user_request, user_sync
- [x] WiFi commands (3): wifiadd, wifipromote, wificonnect

### Phase 4: Automation Migration
- [x] Automation commands (6): automation_add, automation_enable_disable, automation_delete, automation_run, automation (dispatcher), autolog

### Phase 5: Sensor/Peripheral Migration
- [x] i2csensor-pca9685.cpp (3): servo, servoprofile, pwm
- [x] i2csensor-rda5807.cpp (1): fmradio
- [x] i2csensor-apds9960.cpp (1): apdsmode
- [x] System_ESPNow_Sensors.cpp (1): espnow_sensorstream
- [x] System_Power.cpp (1): power
- [x] System_NeoPixel.cpp (1): ledeffect
- [x] System_LLM.cpp (2): llm_load, llm_generate
- [x] System_ESPSR.cpp (5): sr_cmds_add, sr_confidence, sr_accept, sr_dyngain, sr_snip_config
- [x] System_SensorLogging.cpp (1): sensorlog (dispatcher with 8 subcommands)

### Phase 6: Debug & Settings Migration
- [x] System_Debug.cpp (~50 debug flag handlers): debughttp, debugsse, debugcli, debugsensors*, debugcamera, debugmicrophone, debugi2c, debugwifi, debugstorage, debuglogger, debugautomations, debugperformance, debugauth, debugespnow, debugbluetooth*, debugdatetime, debugverbose, debugmaps*, debugthermal, debugtof, debuggamepad, debugapds*, debuggps, debugpresence, debugllm, debugsr, outdisplay, log (dispatcher)
- [x] System_Settings.cpp (2): outserial, outweb

### Phase 7: Remaining Commands
- [x] System_Filesystem.cpp (1): filerename
- [x] System_I2C.cpp (1): sensorautostart
- [x] System_Utils.cpp (1): login
- [x] System_FeatureRegistry.cpp (1): features
- [x] System_ImageManager.cpp (1): imagesend

### Phase 8: Maps Commands (missed in original plan)
- [x] System_Maps.cpp (6): mapload, search, gpstrack, waypoint, waypointfile, waypointfiles
  - Also fixed pre-existing bug in cmd_waypointfiles where pointer arithmetic skipped the first arg (wpName was never read)

### Final Validation
- [ ] Build test
- [ ] Functional test via CLI
- [ ] Performance validation (ensure no regression)

## Code Reduction Estimate

**Before**: ~800 lines of repetitive parsing logic
**After**: ~150 lines in `CommandArgs` class + cleaner command handlers
**Net reduction**: ~650 lines of code
**Maintenance benefit**: Parsing bugs fixed once, not 200+ times

## Benefits Summary

1. **Code quality**: Eliminate ~650 lines of duplication
2. **Consistency**: Uniform error messages and validation
3. **Type safety**: Centralized MAC/int/bool parsing with validation
4. **Maintainability**: Parsing bugs fixed once, not per-command
5. **AI tokenization**: Uniform patterns easier for AI to learn
6. **Developer experience**: Easier to write new commands

## Example Transformations

### Example 1: Simple 2-arg command
```cpp
// BEFORE (espnowpair): 15 lines of parsing
String args = argsInput;
args.trim();
int firstSpace = args.indexOf(' ');
if (firstSpace < 0) return "Usage: espnow pair <mac> <name>";
String macStr = args.substring(0, firstSpace);
String name = args.substring(firstSpace + 1);
macStr.trim();
name.trim();
if (macStr.length() == 0 || name.length() == 0) {
    return "Usage: espnow pair <mac> <name>";
}
uint8_t mac[6];
if (!parseMacAddress(macStr, mac)) {
    return "Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
}

// AFTER: 5 lines
CommandArgs args(argsInput);
if (!args.hasMinArgs(2)) return "Usage: espnowpair <mac> <name>";
uint8_t mac[6];
if (!args.getMac(0, mac)) return "Invalid MAC address format";
String name = args.getArg(1);
```

### Example 2: Multi-arg command with optional params
```cpp
// BEFORE (wifiadd): 20+ lines of nested parsing
String args = originalCmd;
args.trim();
int sp1 = args.indexOf(' ');
if (sp1 <= 0) return "Usage: wifiadd <ssid> <pass> [priority] [hidden0|1]";
String ssid = args.substring(0, sp1);
String rest = args.substring(sp1 + 1);
rest.trim();
int sp2 = rest.indexOf(' ');
String pass = (sp2 < 0) ? rest : rest.substring(0, sp2);
String more = (sp2 < 0) ? "" : rest.substring(sp2 + 1);
more.trim();
int pri = 0;
bool hid = false;
if (more.length() > 0) {
    int sp3 = more.indexOf(' ');
    String priStr = (sp3 < 0) ? more : more.substring(0, sp3);
    pri = priStr.toInt();
    if (pri <= 0) pri = 1;
    String hidStr = (sp3 < 0) ? "" : more.substring(sp3 + 1);
    hid = (hidStr == "1" || hidStr == "true");
}

// AFTER: 6 lines
CommandArgs args(argsInput);
if (!args.hasMinArgs(2)) return "Usage: wifiadd <ssid> <pass> [priority] [hidden0|1]";
String ssid = args.getArg(0);
String pass = args.getArg(1);
int pri = args.getInt(2, 1);  // Default priority = 1
bool hid = args.getBool(3, false);  // Default hidden = false
```

### Example 3: Key=value parsing (automation)
```cpp
// BEFORE: 70+ lines of custom getVal() lambda

// AFTER: Simple key extraction
CommandArgs args(argsInput);
String name = args.getValue("name");
String type = args.getValue("type");
String timeS = args.getValue("time");
String command = args.getValue("command");
bool enabled = args.getValue("enabled") == "true";
```

## Next Steps for Tomorrow

1. **Start with Phase 1**: Create `CommandArgs` class skeleton
2. **Implement core methods**: Constructor, `getArg()`, `getArgCount()`, `hasMinArgs()`
3. **Add typed helpers**: `getInt()`, `getBool()`, `getMac()`
4. **Test compilation**: Ensure no breaking changes
5. **Migrate first batch**: 5-10 simple ESP-NOW commands as proof-of-concept
6. **Validate**: Build test + functional CLI test

## Files to Modify

### Core Implementation
- `System_Command.h` - Add `CommandArgs` class declaration
- `System_Command.cpp` - Implement `CommandArgs` methods

### Migration Targets (in priority order)
1. `System_ESPNow.cpp` (~50 commands)
2. `System_User.cpp` (~15 commands)
3. `System_WiFi.cpp` (~10 commands)
4. `System_Automation.cpp` (~8 commands)
5. `i2csensor-pca9685.cpp` (~5 commands)
6. `i2csensor-rda5807.cpp` (~3 commands)
7. Other sensor files as needed

## Success Criteria

- [x] CommandArgs class implemented (fixed-size array, no heap allocation)
- [x] All ~100+ command handlers migrated to use CommandArgs
- [ ] All migrated commands compile without errors
- [ ] All migrated commands function identically to before
- [ ] No performance regression
