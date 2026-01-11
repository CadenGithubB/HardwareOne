# Error Message Uniformity - Implementation Proposal

## Standard Format
```
"[Module] Error: <description>"
```

## Files Requiring Changes

### 1. i2csensor-apds9960.cpp (Already Updated)
✅ All APDS errors now use `[APDS] Error:` format

### 2. i2csensor-bno055.cpp (IMU Sensor)
**Current errors:**
- "IMU sensor not connected"
- "IMU sensor not started. Use 'imustart' first."
- "IMU sensor initialization failed."

**Proposed changes:**
- "[IMU] Error: Sensor not connected - check wiring"
- "[IMU] Error: Sensor not started - use 'imustart' first"
- "[IMU] Error: Failed to initialize BNO055 sensor"

**Impact:** 8 error messages

### 3. i2csensor-mlx90640.cpp (Thermal Sensor)
**Current errors:**
- "Thermal sensor not connected"
- "Thermal sensor not started. Use 'thermalstart' first."
- "ERROR: Failed to initialize MLX90640 sensor"

**Proposed changes:**
- "[Thermal] Error: Sensor not connected - check wiring"
- "[Thermal] Error: Sensor not started - use 'thermalstart' first"
- "[Thermal] Error: Failed to initialize MLX90640 sensor"

**Impact:** 12 error messages

### 4. i2csensor-vl53l4cx.cpp (ToF Sensor)
**Current errors:**
- "ToF sensor not connected. Check wiring."
- "ToF sensor not started. Use 'tofstart' first."
- "ToF sensor initialization failed."

**Proposed changes:**
- "[ToF] Error: Sensor not connected - check wiring"
- "[ToF] Error: Sensor not started - use 'tofstart' first"
- "[ToF] Error: Failed to initialize VL53L4CX sensor"

**Impact:** 8 error messages

### 5. i2csensor-pa1010d.cpp (GPS Sensor)
**Current errors:**
- "GPS module not connected or initialized"
- "Failed to enqueue GPS start (queue full)"
- "Error: Debug buffer unavailable"

**Proposed changes:**
- "[GPS] Error: Module not connected or initialized"
- "[GPS] Error: Failed to enqueue start (queue full)"
- "[GPS] Error: Debug buffer unavailable"

**Impact:** 6 error messages

### 6. i2csensor-seesaw.cpp (Gamepad Sensor)
**Current errors:**
- "Gamepad not connected"
- "Failed to enqueue Gamepad start (queue full)"

**Proposed changes:**
- "[Gamepad] Error: Not connected - check wiring"
- "[Gamepad] Error: Failed to enqueue start (queue full)"

**Impact:** 4 error messages

### 7. i2csensor-rda5807.cpp (FM Radio)
**Current errors:**
- "ERROR: Frequency must be between 76.0 and 108.0 MHz"
- "ERROR: FM Radio not initialized"
- "ERROR: Volume must be 0-15"

**Proposed changes:**
- "[FM Radio] Error: Frequency must be 76.0-108.0 MHz"
- "[FM Radio] Error: Not initialized - use 'fmradio start' first"
- "[FM Radio] Error: Volume must be 0-15"

**Impact:** 8 error messages

### 8. i2csensor-pca9685.cpp (Servo/PWM)
**Current errors:**
- "Error: PCA9685 not found at 0x40"
- "Error: Channel must be 0-15"
- "Error: Angle must be 0-180"
- "Error: Debug buffer unavailable"

**Proposed changes:**
- "[Servo] Error: PCA9685 not found at 0x40 - check wiring"
- "[Servo] Error: Channel must be 0-15"
- "[Servo] Error: Angle must be 0-180"
- "[Servo] Error: Debug buffer unavailable"

**Impact:** 16 error messages

### 9. System_WiFi.cpp
**Current errors:**
- "WiFi not connected"
- "Invalid network index"
- "Failed to connect to WiFi"

**Proposed changes:**
- "[WiFi] Error: Not connected"
- "[WiFi] Error: Invalid network index"
- "[WiFi] Error: Failed to connect"

**Impact:** 17 error messages

### 10. Optional_Bluetooth.cpp
**Current errors:**
- "Bluetooth not initialized"
- "No active connections"

**Proposed changes:**
- "[Bluetooth] Error: Not initialized - use 'blestart' first"
- "[Bluetooth] Error: No active connections"

**Impact:** 5 error messages

### 11. System_ESPNow.cpp
**Current errors:**
- "ESP-NOW not initialized"
- "Invalid peer MAC address"
- "Failed to send message"

**Proposed changes:**
- "[ESP-NOW] Error: Not initialized - use 'espnow init' first"
- "[ESP-NOW] Error: Invalid peer MAC address"
- "[ESP-NOW] Error: Failed to send message"

**Impact:** 39 error messages

### 12. System_Automation.cpp
**Current errors:**
- "Automation not found"
- "Invalid automation ID"

**Proposed changes:**
- "[Automation] Error: Not found"
- "[Automation] Error: Invalid automation ID"

**Impact:** 7 error messages

### 13. filesystem.cpp
**Current errors:**
- "File not found"
- "Failed to open file"

**Proposed changes:**
- "[FS] Error: File not found"
- "[FS] Error: Failed to open file"

**Impact:** 7 error messages

### 14. System_User.cpp
**Current errors:**
- "User not found"
- "Invalid credentials"

**Proposed changes:**
- "[Auth] Error: User not found"
- "[Auth] Error: Invalid credentials"

**Impact:** 15 error messages

### 15. settings.cpp
**Current errors:**
- "Setting not found"
- "Invalid value"

**Proposed changes:**
- "[Settings] Error: Setting not found"
- "[Settings] Error: Invalid value"

**Impact:** 7 error messages

## Total Impact Summary
- **Files to modify:** 15
- **Error messages to update:** ~150
- **Lines changed:** ~150 (string literals only, no logic changes)
- **Risk level:** LOW (search/replace safe, no functional changes)

## Implementation Strategy
1. Process files one at a time to avoid conflicts
2. Use multi_edit for batch changes within each file
3. Preserve existing error detail/context
4. Test compilation after each file

## Benefits
- ✅ Consistent user experience across all commands
- ✅ Easier to parse programmatically (web UI, automation)
- ✅ Clear module identification in logs
- ✅ Professional, polished error messages

## Next Steps
**Please review and approve this proposal. Once approved, I will implement the changes file-by-file.**
