# CommandArgs Implementation Skeleton

## Quick Start for Tomorrow

This file contains the complete implementation skeleton ready to paste into the codebase.

---

## File 1: System_Command.h (Add to existing file)

Add this class declaration after the existing `CommandEntry` struct:

```cpp
// ============================================================================
// Command Argument Parser
// ============================================================================

/**
 * Unified command argument parser to eliminate fragmented parsing logic.
 * 
 * Usage examples:
 * 
 * // Simple positional args
 * CommandArgs args(argsInput);
 * if (!args.hasMinArgs(2)) return "Usage: cmd <arg1> <arg2>";
 * String arg1 = args.getArg(0);
 * String arg2 = args.getArg(1);
 * 
 * // Typed extraction
 * int value = args.getInt(0, 100);  // Default 100 if missing/invalid
 * bool flag = args.getBool(1, false);
 * 
 * // MAC address parsing
 * uint8_t mac[6];
 * if (!args.getMac(0, mac)) return "Invalid MAC address";
 * 
 * // Remaining text after N args (for freeform text)
 * String message = args.getRemainingAfter(2);  // Everything after arg 2
 * 
 * // Key=value parsing (automation style)
 * String name = args.getValue("name");
 * bool enabled = args.getValue("enabled") == "true";
 */
class CommandArgs {
public:
    // Constructor - parses input string into arguments
    explicit CommandArgs(const String& input);
    
    // Core positional argument access
    String getArg(int index) const;              // Get nth space-separated arg (0-indexed)
    int getArgCount() const;                     // Total argument count
    bool hasMinArgs(int count) const;            // Validation: check minimum args
    bool hasArg(int index) const;                // Check if arg exists
    
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
    
    void parsePositional();                      // Split by spaces, handle quotes
    int findKeyValueStart(const String& key) const;  // Helper for getValue()
};
```

---

## File 2: System_Command.cpp (Add to existing file)

Add this implementation at the end of the file:

```cpp
// ============================================================================
// Command Argument Parser Implementation
// ============================================================================

CommandArgs::CommandArgs(const String& input) : original(input) {
    parsePositional();
}

void CommandArgs::parsePositional() {
    String trimmed = original;
    trimmed.trim();
    
    if (trimmed.length() == 0) {
        return;
    }
    
    int pos = 0;
    while (pos < (int)trimmed.length()) {
        // Skip leading spaces
        while (pos < (int)trimmed.length() && trimmed[pos] == ' ') {
            pos++;
        }
        
        if (pos >= (int)trimmed.length()) break;
        
        // Check for quoted string
        if (trimmed[pos] == '"') {
            pos++;  // Skip opening quote
            int start = pos;
            // Find closing quote
            while (pos < (int)trimmed.length() && trimmed[pos] != '"') {
                pos++;
            }
            args.push_back(trimmed.substring(start, pos));
            if (pos < (int)trimmed.length()) pos++;  // Skip closing quote
        } else {
            // Unquoted argument - find next space
            int start = pos;
            while (pos < (int)trimmed.length() && trimmed[pos] != ' ') {
                pos++;
            }
            args.push_back(trimmed.substring(start, pos));
        }
    }
}

String CommandArgs::getArg(int index) const {
    if (index < 0 || index >= (int)args.size()) {
        return String("");
    }
    return args[index];
}

int CommandArgs::getArgCount() const {
    return (int)args.size();
}

bool CommandArgs::hasMinArgs(int count) const {
    return (int)args.size() >= count;
}

bool CommandArgs::hasArg(int index) const {
    return index >= 0 && index < (int)args.size();
}

int CommandArgs::getInt(int index, int defaultVal) const {
    if (!hasArg(index)) {
        return defaultVal;
    }
    String arg = args[index];
    arg.trim();
    if (arg.length() == 0) {
        return defaultVal;
    }
    // toInt() returns 0 for invalid strings, so we need to check
    // if the string actually represents 0 or is just invalid
    if (arg == "0") {
        return 0;
    }
    int val = arg.toInt();
    if (val == 0 && arg != "0") {
        return defaultVal;  // Invalid conversion
    }
    return val;
}

bool CommandArgs::getBool(int index, bool defaultVal) const {
    if (!hasArg(index)) {
        return defaultVal;
    }
    String arg = args[index];
    arg.trim();
    arg.toLowerCase();
    
    if (arg == "1" || arg == "true" || arg == "on" || arg == "yes") {
        return true;
    }
    if (arg == "0" || arg == "false" || arg == "off" || arg == "no") {
        return false;
    }
    return defaultVal;
}

bool CommandArgs::getMac(int index, uint8_t* mac) const {
    if (!hasArg(index)) {
        return false;
    }
    // Use existing parseMacAddress helper from System_ESPNow.cpp
    extern bool parseMacAddress(const String& macStr, uint8_t* mac);
    return parseMacAddress(args[index], mac);
}

String CommandArgs::getRemainingAfter(int argIndex) const {
    if (argIndex < 0 || argIndex >= (int)args.size()) {
        return String("");
    }
    
    // Find position in original string where arg[argIndex+1] starts
    // This preserves spacing and formatting of remaining text
    String trimmed = original;
    trimmed.trim();
    
    int pos = 0;
    int currentArg = 0;
    
    // Skip past argIndex arguments
    while (currentArg <= argIndex && pos < (int)trimmed.length()) {
        // Skip spaces
        while (pos < (int)trimmed.length() && trimmed[pos] == ' ') {
            pos++;
        }
        
        if (pos >= (int)trimmed.length()) break;
        
        // Skip current argument
        if (trimmed[pos] == '"') {
            pos++;  // Skip opening quote
            while (pos < (int)trimmed.length() && trimmed[pos] != '"') {
                pos++;
            }
            if (pos < (int)trimmed.length()) pos++;  // Skip closing quote
        } else {
            while (pos < (int)trimmed.length() && trimmed[pos] != ' ') {
                pos++;
            }
        }
        
        currentArg++;
    }
    
    // Skip trailing spaces after last arg
    while (pos < (int)trimmed.length() && trimmed[pos] == ' ') {
        pos++;
    }
    
    if (pos >= (int)trimmed.length()) {
        return String("");
    }
    
    String remaining = trimmed.substring(pos);
    remaining.trim();
    return remaining;
}

int CommandArgs::findKeyValueStart(const String& key) const {
    String searchKey = key + "=";
    int pos = original.indexOf(searchKey);
    if (pos < 0) {
        return -1;
    }
    return pos + searchKey.length();
}

String CommandArgs::getValue(const String& key) const {
    int start = findKeyValueStart(key);
    if (start < 0) {
        return String("");
    }
    
    // Skip leading whitespace
    while (start < (int)original.length() && original[start] == ' ') {
        start++;
    }
    
    if (start >= (int)original.length()) {
        return String("");
    }
    
    // Check if value is quoted
    if (original[start] == '"') {
        start++;  // Skip opening quote
        int end = original.indexOf('"', start);
        if (end < 0) {
            end = original.length();  // No closing quote, take rest
        }
        return original.substring(start, end);
    } else {
        // Unquoted value - find next parameter (key=) or end of string
        int end = start;
        while (end < (int)original.length()) {
            // Look ahead for next key=value pattern
            if (original[end] == ' ') {
                int nextSpace = end + 1;
                while (nextSpace < (int)original.length() && original[nextSpace] == ' ') {
                    nextSpace++;
                }
                if (nextSpace < (int)original.length()) {
                    int nextEquals = original.indexOf('=', nextSpace);
                    int nextSpaceAfter = original.indexOf(' ', nextSpace);
                    if (nextEquals > 0 && (nextSpaceAfter < 0 || nextEquals < nextSpaceAfter)) {
                        // Found next parameter
                        break;
                    }
                }
            }
            end++;
        }
        
        String result = original.substring(start, end);
        result.trim();
        return result;
    }
}

bool CommandArgs::hasKey(const String& key) const {
    return findKeyValueStart(key) >= 0;
}
```

---

## Migration Example: espnowpair

### Before (15 lines of parsing):
```cpp
const char* cmd_espnow_pair(const String& argsInput) {
    RETURN_VALID_IF_VALIDATE_CSTR();
    if (!gEspNow) return "Error: ESP-NOW not initialized";
    if (!gEspNow->initialized) {
        return "ESP-NOW not initialized. Run 'openespnow' first.";
    }

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
    
    // ... rest of function
}
```

### After (5 lines of parsing):
```cpp
const char* cmd_espnow_pair(const String& argsInput) {
    RETURN_VALID_IF_VALIDATE_CSTR();
    if (!gEspNow) return "Error: ESP-NOW not initialized";
    if (!gEspNow->initialized) {
        return "ESP-NOW not initialized. Run 'openespnow' first.";
    }

    CommandArgs args(argsInput);
    if (!args.hasMinArgs(2)) return "Usage: espnowpair <mac> <name>";
    
    uint8_t mac[6];
    if (!args.getMac(0, mac)) {
        return "Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
    }
    String name = args.getArg(1);
    
    // ... rest of function (unchanged)
}
```

---

## First 10 Commands to Migrate (Easy wins)

1. **espnowpair** - 2 args (mac, name)
2. **espnowunpair** - 1 arg (name_or_mac)
3. **espnowsend** - 2+ args (target, message...)
4. **espnowbroadcast** - 1+ args (message...)
5. **espnowsetname** - 1 arg (name)
6. **espnowroom** - 1 arg (room)
7. **espnowzone** - 1 arg (zone)
8. **espnowtags** - 1 arg (tags)
9. **espnowfriendlyname** - 1 arg (name)
10. **espnowstationary** - 1 arg (0|1)

These are all simple 1-2 argument commands with straightforward parsing.

---

## Testing Checklist

After implementing CommandArgs:

1. **Compilation test**: `idf.py build`
2. **Unit tests** (create simple test cases):
   - Empty input → 0 args
   - Single arg → 1 arg
   - Multiple args → correct count
   - Quoted strings → preserved
   - MAC parsing → valid/invalid cases
   - Int parsing → valid/invalid/defaults
   - Bool parsing → all variants (1/0/true/false/on/off)
   - getRemainingAfter → correct substring
   - getValue → key=value extraction

3. **Functional tests** (via CLI):
   - Test each migrated command with valid inputs
   - Test each migrated command with invalid inputs
   - Verify error messages are correct

---

## Notes for Tomorrow

- Start by adding the class to `System_Command.h`
- Implement in `System_Command.cpp`
- Build test to ensure no compilation errors
- Migrate first 5 simple commands as proof-of-concept
- Test those 5 commands thoroughly
- If successful, continue with remaining ESP-NOW commands
- Track line count reduction as you go

**Estimated time**: 
- Phase 1 (CommandArgs class): 1-2 hours
- First 10 command migrations: 1-2 hours
- Testing: 30 minutes
- Total Day 1: 3-4 hours

Good luck! 🚀
