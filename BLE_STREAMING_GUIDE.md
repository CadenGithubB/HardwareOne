# Bluetooth LE Data Streaming System

## Overview

The BLE system now provides **bidirectional communication** with **continuous data streaming** capabilities. This allows your device to push sensor data, system status, and event notifications to connected clients (like smart glasses or phones) automatically.

## Architecture

### Services & Characteristics

**1. Command Service** (`12345678-1234-5678-1234-56789abcdef0`)
- `CMD_REQUEST` (Write) - Client sends commands
- `CMD_RESPONSE` (Notify) - Device sends command responses
- `CMD_STATUS` (Read) - Connection status

**2. Data Streaming Service** (`12345678-1234-5678-1234-56789abcdef1`) ✨ NEW
- `SENSOR_DATA` (Notify) - Continuous sensor updates
- `SYSTEM_STATUS` (Notify) - System health updates
- `EVENT_NOTIFY` (Notify) - Important event notifications
- `STREAM_CONTROL` (Write) - Control streaming behavior

**3. Device Info Service** (Standard 0x180A)
- Manufacturer, Model, Firmware version

---

## Commands

### 1. Manual Message Sending

```bash
blesend <message>
```
Send any text message to connected BLE client.

**Example:**
```bash
blesend Hello from device!
blesend {"custom":"json","data":123}
```

### 2. Stream Control

```bash
blestream                    # Show current streaming status
blestream on                 # Enable all streams
blestream off                # Disable all streams
blestream sensors            # Enable sensor stream only
blestream sensors off        # Disable sensor stream
blestream system             # Enable system stream only
blestream system off         # Disable system stream
blestream events             # Enable event stream only
blestream events off         # Disable event stream
blestream interval 500 2000  # Set intervals (sensor=500ms, system=2000ms)
```

**Status Output:**
```
Streaming: sensors=ON system=ON events=OFF
Intervals: sensor=1000ms system=5000ms
Stats: sensors=1234 system=567 events=89
```

### 3. Event Notifications

```bash
bleevent <message>
```
Send custom event notification to BLE client.

**Example:**
```bash
bleevent Button pressed
bleevent Temperature threshold exceeded
bleevent {"event":"gesture","type":"swipe_left"}
```

---

## API Usage (C++)

### Sending Data

```cpp
// 1. Send manual message
if (isBLEConnected()) {
    const char* msg = "Hello from device!";
    sendBLEResponse(msg, strlen(msg));
}

// 2. Push sensor data (JSON)
char sensorJson[256];
snprintf(sensorJson, sizeof(sensorJson),
         "{\"temp\":%.1f,\"distance\":%d}",
         temperature, distance);
blePushSensorData(sensorJson, strlen(sensorJson));

// 3. Push system status (JSON)
char statusJson[128];
snprintf(statusJson, sizeof(statusJson),
         "{\"heap\":%lu,\"uptime\":%lu}",
         ESP.getFreeHeap(), millis()/1000);
blePushSystemStatus(statusJson, strlen(statusJson));

// 4. Push event notification
blePushEvent(BLE_EVENT_BUTTON_PRESS, "Button A pressed", "duration=250ms");
blePushEvent(BLE_EVENT_CUSTOM, "Custom event", nullptr);
```

### Stream Control

```cpp
// Enable/disable streams
bleEnableStream(BLE_STREAM_SENSORS);
bleEnableStream(BLE_STREAM_SYSTEM);
bleEnableStream(BLE_STREAM_EVENTS);
bleEnableStream(BLE_STREAM_ALL);

bleDisableStream(BLE_STREAM_SENSORS);
bleDisableStream(BLE_STREAM_ALL);

// Set update intervals
bleSetStreamInterval(1000, 5000);  // sensor=1s, system=5s

// Check if streaming is enabled
if (bleIsStreamEnabled(BLE_STREAM_SENSORS)) {
    // Sensor streaming is active
}

// Auto-update (call from main loop)
bleUpdateStreams();  // Automatically sends data at configured intervals
```

### Event Types

```cpp
enum BLEEventType {
    BLE_EVENT_SENSOR_CONNECTED,      // Sensor connected
    BLE_EVENT_SENSOR_DISCONNECTED,   // Sensor disconnected
    BLE_EVENT_LOW_BATTERY,           // Low battery warning
    BLE_EVENT_WIFI_CONNECTED,        // WiFi connected
    BLE_EVENT_WIFI_DISCONNECTED,     // WiFi disconnected
    BLE_EVENT_BUTTON_PRESS,          // Button pressed
    BLE_EVENT_GESTURE_DETECTED,      // Gesture detected
    BLE_EVENT_THRESHOLD_EXCEEDED,    // Threshold exceeded
    BLE_EVENT_ERROR,                 // Error occurred
    BLE_EVENT_CUSTOM                 // Custom event
};
```

---

## Data Formats

### Sensor Data Stream (JSON)

```json
{
  "sensors": {
    "thermal": {"min":20.5, "max":35.2, "center":28.3, "valid":true},
    "tof": {"dist_mm":1234, "valid":true},
    "imu": {"heading":45.2, "pitch":10.1, "roll":-5.3, "valid":true}
  },
  "ts": 123456
}
```

### System Status Stream (JSON)

```json
{
  "system": {
    "heap_free": 102400,
    "heap_min": 95000,
    "psram_free": 2020000,
    "uptime": 3600
  },
  "ts": 123456
}
```

### Event Notification (JSON)

```json
{
  "type": 6,
  "msg": "Button pressed",
  "details": "duration=250ms",
  "ts": 123456
}
```

---

## Integration Examples

### 1. Auto-Push Sensor Data Every Second

```cpp
void setup() {
    initBluetooth();
    startBLEAdvertising();
    
    // Enable sensor streaming at 1 second intervals
    bleSetStreamInterval(1000, 5000);
    bleEnableStream(BLE_STREAM_SENSORS);
}

void loop() {
    bleUpdateStreams();  // Automatically sends sensor data
    // ... rest of loop
}
```

### 2. Push Events on Button Press

```cpp
void handleButtonPress() {
    if (isBLEConnected()) {
        blePushEvent(BLE_EVENT_BUTTON_PRESS, 
                     "Button A", 
                     "short_press");
    }
}
```

### 3. Push Custom Sensor Reading

```cpp
void onTemperatureChange(float temp) {
    if (isBLEConnected() && temp > 30.0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "High temp: %.1f°C", temp);
        blePushEvent(BLE_EVENT_THRESHOLD_EXCEEDED, msg);
    }
}
```

### 4. Reusable Data Pipeline

```cpp
// Create a reusable function to push any sensor data
void pushSensorUpdate() {
    if (!isBLEConnected()) return;
    
    char json[512];
    int pos = snprintf(json, sizeof(json), "{");
    
    // Add thermal data
    if (thermalEnabled && thermalConnected) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "\"thermal\":{\"temp\":%.1f},", 
                        thermalCenterTemp);
    }
    
    // Add distance data
    if (tofEnabled && tofConnected) {
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "\"distance\":%d,", 
                        tofDistance);
    }
    
    // Remove trailing comma and close
    if (json[pos-1] == ',') pos--;
    pos += snprintf(json + pos, sizeof(json) - pos, "}");
    
    blePushSensorData(json, pos);
}
```

---

## Phone/Client Side (Pseudo-code)

```javascript
// Connect to device
const device = await navigator.bluetooth.requestDevice({
    filters: [{ name: 'HardwareOne' }],
    optionalServices: [
        '12345678-1234-5678-1234-56789abcdef0',  // Command service
        '12345678-1234-5678-1234-56789abcdef1'   // Data service
    ]
});

const server = await device.gatt.connect();

// Subscribe to sensor data stream
const dataService = await server.getPrimaryService(
    '12345678-1234-5678-1234-56789abcdef1'
);
const sensorChar = await dataService.getCharacteristic(
    '12345678-1234-5678-1234-56789abcde11'
);

await sensorChar.startNotifications();
sensorChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = new TextDecoder().decode(event.target.value);
    const json = JSON.parse(data);
    console.log('Sensor data:', json);
    // Update UI with sensor data
});

// Subscribe to events
const eventChar = await dataService.getCharacteristic(
    '12345678-1234-5678-1234-56789abcde13'
);
await eventChar.startNotifications();
eventChar.addEventListener('characteristicvaluechanged', (event) => {
    const data = new TextDecoder().decode(event.target.value);
    const json = JSON.parse(data);
    console.log('Event:', json);
    // Show notification
});

// Send command
const cmdService = await server.getPrimaryService(
    '12345678-1234-5678-1234-56789abcdef0'
);
const cmdChar = await cmdService.getCharacteristic(
    '12345678-1234-5678-1234-56789abcde01'
);
await cmdChar.writeValue(new TextEncoder().encode('status'));
```

---

## Future: Smart Glasses Protocol

When you get your smart glasses and understand their protocol:

1. **Add custom characteristics** for glasses-specific data
2. **Create protocol adapter** to translate between formats
3. **Implement glasses commands** (display text, show icons, etc.)
4. **Add gesture recognition** integration

Example structure for future expansion:

```cpp
// Future: Glasses-specific service
#define BLE_GLASSES_SERVICE_UUID "12345678-1234-5678-1234-56789abcdef2"
#define BLE_GLASSES_DISPLAY_CHAR "..."  // Send display commands
#define BLE_GLASSES_GESTURE_CHAR "..."  // Receive gesture input
#define BLE_GLASSES_BATTERY_CHAR "..."  // Read battery level

// Future: Protocol adapter
void sendToGlasses(const char* text, uint8_t x, uint8_t y) {
    // Translate to glasses protocol and send
}
```

---

## Benefits

✅ **Modular** - Separate characteristics for different data types  
✅ **Reusable** - Pipeline functions work for any sensor  
✅ **Customizable** - Control what streams and when  
✅ **Low-level access** - Full control over BLE stack  
✅ **Event-driven** - Push notifications for important events  
✅ **Scalable** - Easy to add new data types or protocols  

---

## Files Modified

- `Optional_Bluetooth.h` - Added streaming API and enums
- `Optional_Bluetooth.cpp` - Added data service and commands
- `Optional_Bluetooth_Streaming.cpp` - NEW - Streaming implementation
- `hardwareone_sketch.cpp` - Added `bleUpdateStreams()` to main loop

---

## Next Steps

1. ✅ Test `blesend` command
2. ✅ Enable streaming with `blestream on`
3. ✅ Monitor data on phone app
4. ✅ Customize intervals with `blestream interval`
5. 🔮 Integrate with smart glasses when available
