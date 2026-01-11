# ESP-NOW Remote Sensor Streaming - Implementation Summary

## Overview
This document summarizes the implementation of remote sensor data streaming over ESP-NOW, allowing master devices to display sensor data from worker devices in real-time.

## Architecture

### Message Flow
```
Worker Device                    Master Device
    |                                 |
    | 1. Sensor starts                |
    |--[SENSOR_STATUS: enabled]------>|
    |                                 | (Updates cache)
    |                                 |
    | 2. Opt-in streaming enabled     |
    |   (CLI: espnow sensorstream)    |
    |                                 |
    | 3. Periodic data broadcast      |
    |--[SENSOR_DATA: thermal]-------->|
    |   (every 5 seconds)             | (Updates cache, triggers SSE)
    |                                 |
    |                                 | 4. Web client requests
    |                                 |<--[GET /api/sensors/remote]
    |                                 |---[JSON: devices + sensors]-->
    |                                 |
    | 5. Sensor stops                 |
    |--[SENSOR_STATUS: disabled]----->|
    |                                 | (Invalidates cache)
```

### Components

#### 1. Data Structures (`espnow_sensors.h/cpp`)
- **RemoteSensorData**: Cache entry for each device+sensor combination
- **RemoteSensorType**: Enum for all sensor types (thermal, ToF, IMU, GPS, gamepad, fmradio)
- **Cache**: 48 entries (8 devices × 6 sensors), 30-second TTL

#### 2. Message Types
- **SENSOR_STATUS**: Broadcast when sensor starts/stops (small, ~100 bytes)
- **SENSOR_DATA**: Periodic sensor data (fragmented for large payloads)

#### 3. API Endpoints
- `GET /api/sensors/remote` - List all remote devices with sensors
- `GET /api/sensors/remote?device=<mac>&sensor=<type>` - Get specific sensor data

#### 4. CLI Commands
- `espnow sensorstream <sensor> <on|off>` - Enable/disable streaming (worker only)
- `espnow sensorstatus` - Show streaming status

## Heap Usage Analysis

### Static Memory (Always Allocated)

**Remote Sensor Cache:**
```
RemoteSensorData structure:
  - deviceMac[6]:        6 bytes
  - deviceName[32]:     32 bytes
  - sensorType:          4 bytes (enum)
  - jsonData:           24 bytes (String overhead)
  - lastUpdate:          4 bytes (unsigned long)
  - valid:               1 byte (bool)
  - Padding:            ~0 bytes
  --------------------------------
  Total per entry:     ~71 bytes

Cache array: 48 entries (8 devices × 6 sensors)
Total: 48 × 71 = 3,408 bytes (~3.3 KB)
```

**Streaming State (Worker devices only):**
```
  - gSensorStreamingEnabled[6]:    6 bytes (bool array)
  - gLastSensorBroadcast[6]:      24 bytes (unsigned long array)
  --------------------------------
  Total:                          30 bytes
```

**Static Total: ~3.4 KB**

### Dynamic Memory (Per Active Remote Sensor)

**Cached JSON Data (String content):**
```
Sensor Type    | Typical Size | Max Size  | Notes
---------------|--------------|-----------|---------------------------
Thermal        | 2,500 bytes  | 3,000 B   | 768 integers (no decimals)
ToF            | 400 bytes    | 500 B     | 4 objects with metadata
IMU            | 250 bytes    | 300 B     | Orientation + calibration
GPS            | 300 bytes    | 400 B     | Position + satellites
Gamepad        | 80 bytes     | 100 B     | X/Y/buttons
FM Radio       | 150 bytes    | 200 B     | Frequency + RDS
```

**Scenario Analysis:**

1. **Minimum (No remote sensors active):**
   - Static cache only: **3.4 KB**

2. **Typical (3 worker devices, thermal + IMU each):**
   - Static: 3.4 KB
   - Dynamic: 3 devices × (2,500 + 250) = 8,250 bytes
   - **Total: ~11.6 KB**

3. **Heavy (5 worker devices, thermal + ToF + IMU each):**
   - Static: 3.4 KB
   - Dynamic: 5 devices × (2,500 + 400 + 250) = 15,750 bytes
   - **Total: ~19 KB**

4. **Maximum (8 worker devices, all 6 sensors each):**
   - Static: 3.4 KB
   - Dynamic: 8 devices × (2,500 + 400 + 250 + 300 + 80 + 150) = 29,440 bytes
   - **Total: ~33 KB**

### Memory Optimization

**Thermal Data Optimization:**
- Original: Float array (768 × 4 bytes = 3,072 bytes raw data)
- Optimized: Integer array (768 × ~4 chars avg = ~3,000 bytes JSON)
- Savings: Minimal in JSON form, but cleaner data representation
- Future: Could use binary protocol for ~60% reduction

**Cache Management:**
- Automatic TTL expiration (30 seconds)
- Invalidation on sensor stop
- LRU eviction if cache fills (48 entry limit)

## Implementation Files

### New Files Created
1. `espnow_sensors.h` - Remote sensor data structures and API
2. `espnow_sensors.cpp` - Remote sensor cache and message handlers
3. `espnow_sensor_commands.cpp` - CLI commands for streaming control

### Modified Files
1. `espnow_system.cpp` - Added SENSOR_STATUS and SENSOR_DATA message handlers
2. `WebPage_Sensors.cpp` - Added `/api/sensors/remote` endpoint
3. `WebCore_Server.h` - Added handleRemoteSensors declaration
4. `WebCore_Server.cpp` - Registered remote sensors endpoint
5. `Sensor_Thermal_MLX90640.cpp` - Added status broadcasting on start/stop
6. `Sensor_IMU_BNO055.cpp` - Added status broadcasting on start/stop
7. `Sensor_ToF_VL53L4CX.cpp` - Added status broadcasting on start/stop
8. `Sensor_Gamepad_Seesaw.cpp` - Added status broadcasting on start/stop

### Integration Points

**Sensor Start (all sensors):**
```cpp
#if ENABLE_ESPNOW
  #include "espnow_sensors.h"
  broadcastSensorStatus(REMOTE_SENSOR_<TYPE>, true);
#endif
```

**Sensor Stop (all sensors):**
```cpp
#if ENABLE_ESPNOW
  #include "espnow_sensors.h"
  broadcastSensorStatus(REMOTE_SENSOR_<TYPE>, false);
#endif
```

## Usage Guide

### Worker Device Setup
```bash
# 1. Enable ESP-NOW and configure as worker
espnow init
set espnow.meshRole 0  # Worker role

# 2. Start desired sensors
thermalstart
imustart

# 3. Enable streaming for specific sensors (opt-in)
espnow sensorstream thermal on
espnow sensorstream imu on

# 4. Check streaming status
espnow sensorstatus
```

### Master Device Setup
```bash
# 1. Enable ESP-NOW and configure as master
espnow init
set espnow.meshRole 1  # Master role

# 2. View remote sensor cache
espnow sensorstatus

# 3. Access via web interface
# Navigate to /sensors page
# Remote sensors section will show all worker device sensors
```

### Web API Usage
```javascript
// List all remote devices with sensors
fetch('/api/sensors/remote')
  .then(r => r.json())
  .then(data => {
    // data.devices = [{mac, name, sensors: [...]}, ...]
  });

// Get specific sensor data
fetch('/api/sensors/remote?device=AA:BB:CC:DD:EE:FF&sensor=thermal')
  .then(r => r.json())
  .then(data => {
    // Thermal data in same format as local sensor
  });
```

## Performance Characteristics

### Network Bandwidth (per worker device)

**Status Messages (on sensor start/stop):**
- Size: ~100 bytes
- Frequency: On-demand (sensor lifecycle events)
- Impact: Negligible

**Data Messages (periodic streaming):**
```
Sensor    | Size      | Interval | Bandwidth
----------|-----------|----------|------------
Thermal   | 3,000 B   | 5 sec    | 600 B/s
ToF       | 500 B     | 5 sec    | 100 B/s
IMU       | 300 B     | 5 sec    | 60 B/s
GPS       | 400 B     | 5 sec    | 80 B/s
Gamepad   | 100 B     | 5 sec    | 20 B/s
FM Radio  | 200 B     | 5 sec    | 40 B/s
```

**Typical Load (thermal + IMU streaming):**
- Combined: 660 bytes/second per worker
- 3 workers: ~2 KB/s total
- Well within ESP-NOW capacity (~250 KB/s)

### Fragmentation

Large messages (>200 bytes) are automatically fragmented by ESP-NOW v2:
- Thermal data: ~15 fragments (3000 / 200)
- Reassembly handled transparently
- Retry logic ensures delivery

## Testing Checklist

- [ ] Worker broadcasts SENSOR_STATUS on sensor start
- [ ] Worker broadcasts SENSOR_STATUS on sensor stop
- [ ] Master receives and caches sensor status
- [ ] Worker streams SENSOR_DATA when enabled
- [ ] Master receives and caches sensor data
- [ ] Master invalidates cache on TTL expiration
- [ ] Web API returns correct device list
- [ ] Web API returns correct sensor data
- [ ] Thermal data uses integer format (no decimals)
- [ ] Large payloads (thermal) fragment correctly
- [ ] CLI commands work on worker devices
- [ ] CLI commands show error on master for streaming
- [ ] Sensors page UI shows remote sensors section
- [ ] Remote sensors section shows "ESP-NOW not enabled" when appropriate

## Future Enhancements

1. **Binary Protocol**: Reduce thermal data to ~1,200 bytes (60% reduction)
2. **Compression**: zlib compression for JSON payloads
3. **Selective Streaming**: Stream only when web client is viewing
4. **Rate Adaptation**: Adjust streaming interval based on network conditions
5. **Historical Data**: Cache last N samples for trend visualization
6. **Aggregation**: Master aggregates data from multiple workers for fleet view

## Conclusion

The remote sensor streaming system adds **~3.4 KB static + 10-30 KB dynamic** heap usage depending on the number of active remote sensors. This is well within ESP32 capabilities and provides real-time sensor data visualization across the mesh network.

The implementation is:
- **Opt-in**: Workers must explicitly enable streaming
- **Efficient**: Uses existing ESP-NOW fragmentation and retry logic
- **Scalable**: Supports up to 8 worker devices with 6 sensors each
- **Robust**: TTL expiration and automatic cache invalidation
- **User-friendly**: Simple CLI commands and web API
