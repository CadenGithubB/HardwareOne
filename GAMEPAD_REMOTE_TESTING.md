# Gamepad Remote Sensor Testing Guide

## Overview
This guide provides step-by-step instructions for testing the ESP-NOW remote sensor streaming system using a gamepad as the test sensor.

## Prerequisites

### Hardware Required
- **2 ESP32 devices** with HardwareOne firmware
- **1 Seesaw Gamepad** (Adafruit ATSAMD09 breakout) connected to Worker device
- Both devices on same WiFi network (or ESP-NOW works without WiFi)

### Firmware Requirements
- Both devices must have:
  - `ENABLE_ESPNOW=1`
  - `ENABLE_GAMEPAD_SENSOR=1`
  - Latest firmware with remote sensor streaming code

## Implementation Details

### What Was Added

**Gamepad Task (`Sensor_Gamepad_Seesaw.cpp:495-509`):**
```cpp
// Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
  #include "espnow_sensors.h"
  extern Settings gSettings;
  extern bool meshEnabled();
  if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
    char gamepadJson[128];
    int jsonLen = snprintf(gamepadJson, sizeof(gamepadJson),
                           "{\"v\":1,\"x\":%d,\"y\":%d,\"buttons\":%lu}",
                           filtX, filtY, (unsigned long)buttons);
    if (jsonLen > 0 && jsonLen < 128) {
      sendSensorDataUpdate(REMOTE_SENSOR_GAMEPAD, String(gamepadJson));
    }
  }
#endif
```

**Key Features:**
- ✅ Only workers stream (master check: `gSettings.meshRole != MESH_ROLE_MASTER`)
- ✅ Only when ESP-NOW enabled (`meshEnabled()`)
- ✅ Rate-limited by `sendSensorDataUpdate()` (5-second default interval)
- ✅ Automatic fragmentation for large payloads (gamepad is small, ~80 bytes)

## Testing Steps

### Phase 1: Device Setup

#### Worker Device (with Gamepad)

```bash
# 1. Connect to worker device serial console
# 2. Initialize ESP-NOW
espnow init

# 3. Configure as worker (role 0)
set espnow.meshRole 0

# 4. Set device name (optional but recommended)
set espnow.espnowDeviceName "Worker-Gamepad"

# 5. Get worker MAC address (note this down)
espnow status
# Look for "My MAC: XX:XX:XX:XX:XX:XX"

# 6. Verify gamepad hardware is connected
i2c scan
# Should see device at 0x50 (Seesaw gamepad)
```

#### Master Device

```bash
# 1. Connect to master device serial console
# 2. Initialize ESP-NOW
espnow init

# 3. Configure as master (role 1)
set espnow.meshRole 1

# 4. Set device name (optional)
set espnow.espnowDeviceName "Master-Display"

# 5. Get master MAC address (note this down)
espnow status
```

### Phase 2: Pairing

#### On Worker Device:
```bash
# Pair with master using its MAC address
espnow pair <MASTER_MAC>
# Example: espnow pair AA:BB:CC:DD:EE:FF

# Verify pairing
espnow status
# Should show master in paired devices list
```

#### On Master Device:
```bash
# Pair with worker using its MAC address
espnow pair <WORKER_MAC>

# Verify pairing
espnow status
# Should show worker in paired devices list
```

### Phase 3: Start Gamepad Sensor

#### On Worker Device:
```bash
# 1. Start the gamepad sensor
gamepadstart

# Wait for initialization (~2-3 seconds)
# You should see: "Gamepad (Seesaw) initialized"

# 2. Verify gamepad is running
espnow sensorstatus

# 3. Enable streaming to master
espnow sensorstream gamepad on

# Expected output: "Started streaming gamepad sensor data to master"

# 4. Test gamepad locally (optional)
gamepad
# Move joystick and press buttons to verify hardware works
```

### Phase 4: Verify on Master

#### Via Serial Console:
```bash
# Check remote sensor status
espnow sensorstatus

# Expected output should show:
# Remote sensor cache:
# {"devices":[{"mac":"XX:XX:XX:XX:XX:XX","name":"Worker-Gamepad","sensors":["gamepad"]}]}
```

#### Via Web Interface:
1. Navigate to `http://<MASTER_IP>/sensors`
2. Scroll down to **"Remote Sensors (ESP-NOW)"** section
3. Should see a card: **"Worker-Gamepad - gamepad"**
4. Card should show: `Loading...` initially, then gamepad data

### Phase 5: Interactive Testing

#### On Worker Device:
1. **Move joystick** - X and Y values should change
2. **Press buttons** (A, B, X, Y, START)
3. Watch serial output for button press events

#### On Master Web Interface:
1. Refresh the sensors page
2. Remote gamepad card should update every ~5 seconds
3. Verify X/Y values match joystick position
4. Verify button states update when pressed

### Phase 6: Debug and Monitoring

#### Enable ESP-NOW Debug Output:
```bash
# On both devices
set debug.espNowStream 1

# On worker - you should see:
# [REMOTE_SENSORS] Sending gamepad data (80 bytes)

# On master - you should see:
# [REMOTE_SENSORS] Updated cache for Worker-Gamepad gamepad (80 bytes)
```

#### Check ESP-NOW Metrics:
```bash
espnow status

# Look for:
# - Messages sent/received counters
# - Fragmentation stats (should be 0 for gamepad - too small to fragment)
# - Retry queue status
```

#### Monitor Streaming Status:
```bash
# On worker
espnow sensorstatus

# Should show streaming is active for gamepad
```

## Expected Behavior

### Normal Operation

**Worker Device:**
- Gamepad polls every ~58ms (default `gamepadDevicePollMs`)
- ESP-NOW sends data every ~5 seconds (rate-limited by `sendSensorDataUpdate`)
- Serial output shows button presses in real-time
- Status broadcasts sent on gamepad start/stop

**Master Device:**
- Receives `SENSOR_STATUS` when worker starts gamepad (~100 bytes, one-time)
- Receives `SENSOR_DATA` every ~5 seconds (~80 bytes each)
- Cache updates with latest gamepad state
- Web interface shows real-time data (via SSE polling)

### Data Format

**Gamepad JSON (sent over ESP-NOW):**
```json
{
  "v": 1,
  "x": 512,
  "y": 512,
  "buttons": 4294967295
}
```

**Fields:**
- `v`: Version (always 1)
- `x`: Joystick X position (0-1023, center ~512)
- `y`: Joystick Y position (0-1023, center ~512)
- `buttons`: Button bitmask (active low, 0xFFFFFFFF = no buttons pressed)

**Button Bits:**
- Bit 5: Button A
- Bit 6: Button B
- Bit 2: Button X
- Bit 3: Button Y
- Bit 16: START
- Bit 0: SELECT

## Troubleshooting

### Issue: "ESP-NOW not enabled or no remote devices found"

**Cause:** ESP-NOW not initialized or devices not paired

**Fix:**
```bash
# On both devices
espnow init
espnow pair <OTHER_DEVICE_MAC>
```

### Issue: Remote sensor shows but no data updates

**Cause:** Streaming not enabled on worker

**Fix:**
```bash
# On worker
espnow sensorstream gamepad on
```

### Issue: "Error: Master devices receive sensor data, they don't stream it"

**Cause:** Trying to enable streaming on master device

**Fix:** Streaming is worker→master only. Masters receive, workers send.

### Issue: Gamepad data is stale (>30 seconds old)

**Cause:** Worker stopped streaming or lost connection

**Fix:**
```bash
# On worker - verify gamepad still running
gamepadstart

# Re-enable streaming
espnow sensorstream gamepad on

# Check ESP-NOW connection
espnow status
```

### Issue: No gamepad detected at 0x50

**Cause:** Hardware not connected or wrong I2C bus

**Fix:**
- Verify Seesaw gamepad is connected to I2C1 (Wire1)
- Check wiring: SDA, SCL, 3.3V, GND
- Try I2C scan: `i2c scan`

## Performance Metrics

### Network Bandwidth (per worker)

**Gamepad Streaming:**
- Message size: ~80 bytes
- Frequency: Every 5 seconds (default)
- Bandwidth: **16 bytes/second**
- Fragmentation: None (message too small)

**Status Messages:**
- Start/Stop: ~100 bytes each (one-time events)
- Negligible impact

### Memory Usage

**Worker Device:**
- Streaming state: 30 bytes static
- No additional dynamic allocation

**Master Device:**
- Cache entry: ~71 bytes static + ~80 bytes dynamic per gamepad
- **Total: ~151 bytes per remote gamepad**

### Latency

- **Sensor read to cache**: <1ms (local I2C)
- **Cache to ESP-NOW send**: ~5 seconds (rate limit)
- **ESP-NOW transmission**: <10ms (typical)
- **Master cache update**: <1ms
- **Web UI update**: 1-2 seconds (SSE polling)
- **End-to-end latency**: ~6-7 seconds

## Advanced Testing

### Test Multiple Sensors

```bash
# On worker with multiple sensors
thermalstart
imustart
gamepadstart

# Enable streaming for all
espnow sensorstream thermal on
espnow sensorstream imu on
espnow sensorstream gamepad on

# Master should show all 3 sensors in remote section
```

### Test TTL Expiration

```bash
# On worker - stop streaming
espnow sensorstream gamepad off

# Wait 30 seconds
# On master web interface - gamepad should disappear or show "Data expired"
```

### Test Mesh Forwarding

```bash
# Add a 3rd device as intermediate hop
# Worker → Intermediate → Master
# Gamepad data should still reach master via mesh routing
```

### Stress Test

```bash
# Enable all sensors on worker
# Enable debug output
set debug.espNowStream 1

# Monitor ESP-NOW metrics
espnow status

# Look for:
# - No dropped messages
# - No queue overflows
# - Successful fragmentation for thermal data
```

## Success Criteria

✅ **Worker device:**
- Gamepad initializes successfully
- Streaming enabled without errors
- Serial shows periodic "Sending gamepad data" messages
- No ESP-NOW errors or queue overflows

✅ **Master device:**
- Remote sensors section visible on web page
- Worker gamepad appears in device list
- Data updates every ~5 seconds
- No "Data expired" errors during active streaming

✅ **Data accuracy:**
- Joystick X/Y values match physical position
- Button presses reflected in bitmask
- No data corruption or invalid JSON

✅ **Performance:**
- <10 seconds end-to-end latency
- No memory leaks or heap fragmentation
- Stable operation over extended periods (>1 hour)

## Next Steps

After successful gamepad testing:

1. **Test other sensors** (thermal, IMU, ToF)
2. **Test multi-device scenarios** (multiple workers → 1 master)
3. **Test mesh routing** (worker → intermediate → master)
4. **Optimize streaming intervals** based on sensor type
5. **Add compression** for large payloads (thermal)
6. **Implement selective streaming** (only when web client viewing)

## Conclusion

The gamepad is an ideal first test because:
- **Small payload** (~80 bytes) - no fragmentation complexity
- **Simple data format** - easy to verify correctness
- **Interactive feedback** - immediate visual confirmation
- **Low bandwidth** - minimal network impact
- **Fast polling** - demonstrates real-time capability

Once gamepad streaming works reliably, the same pattern applies to all other sensors with only minor adjustments for data size and format.
