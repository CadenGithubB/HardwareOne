/**
 * Bluetooth System - ESP32 Built-in BLE Server Implementation
 * 
 * Provides BLE connectivity for smart glasses and external devices.
 * Uses ESP32 Bluedroid stack (built-in) for better compatibility.
 * 
 * Features:
 * - GATT server with custom services
 * - Command service (send commands, receive responses)
 * - Sensor data notifications (push model)
 */

#include "Optional_Bluetooth.h"
#include "System_Utils.h"

#if ENABLE_BLUETOOTH

#include "esp_bt.h"

#include "OLED_Display.h"
#include "OLED_SettingsEditor.h"
#include "System_BuildConfig.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_MemUtil.h"
#include "System_Settings.h"

// External dependencies
extern void broadcastOutput(const char* msg);
extern bool ensureDebugBuffer();

// Memory allocation

// Debug macros
#define BLE_DEBUGF(flag, fmt, ...) \
  do { \
    if (isDebugFlagSet(flag) && ensureDebugBuffer()) { \
      snprintf(getDebugBuffer(), 1024, "[BLE] " fmt, ##__VA_ARGS__); \
      broadcastOutput(getDebugBuffer()); \
    } \
  } while(0)

// =============================================================================
// GLOBAL STATE
// =============================================================================

BLESystemState* gBLEState = nullptr;

// ESP32 BLE objects
static BLEServer* pServer = nullptr;
static BLEAdvertising* pAdvertising = nullptr;

// Services
static BLEService* pCommandService = nullptr;
static BLEService* pDataService = nullptr;
static BLEService* pDeviceInfoService = nullptr;

// Command service characteristics
static BLECharacteristic* pCmdRequestChar = nullptr;
static BLECharacteristic* pCmdResponseChar = nullptr;
static BLECharacteristic* pCmdStatusChar = nullptr;

// Data streaming service characteristics (non-static for access from streaming module)
BLECharacteristic* pSensorDataChar = nullptr;
BLECharacteristic* pSystemStatusChar = nullptr;
BLECharacteristic* pEventNotifyChar = nullptr;
static BLECharacteristic* pStreamControlChar = nullptr;

// Device info characteristics
static BLECharacteristic* pManufacturerChar = nullptr;
static BLECharacteristic* pModelChar = nullptr;
static BLECharacteristic* pFirmwareChar = nullptr;

// Forward declarations
static void processIncomingBLECommand(const char* data, size_t len);

// =============================================================================
// DEVICE TYPE IDENTIFICATION
// =============================================================================

// Convert MAC address to string for comparison
static String macToString(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Convert device type to human-readable string
const char* bleDeviceTypeToString(BLEDeviceType type) {
  switch (type) {
    case BLE_DEVICE_GLASSES_LEFT: return "Glasses (Left)";
    case BLE_DEVICE_GLASSES_RIGHT: return "Glasses (Right)";
    case BLE_DEVICE_RING: return "Smart Ring";
    case BLE_DEVICE_PHONE: return "Phone";
    case BLE_DEVICE_CUSTOM: return "Custom Device";
    default: return "Unknown";
  }
}

// Identify device type by MAC address
BLEDeviceType bleIdentifyDeviceByMAC(const uint8_t* mac) {
  String macStr = macToString(mac);
  macStr.toUpperCase();  // Normalize to uppercase
  
  // Check against known device MACs
  if (gSettings.bleGlassesLeftMAC.length() > 0) {
    String leftMAC = gSettings.bleGlassesLeftMAC;
    leftMAC.toUpperCase();
    if (macStr == leftMAC) return BLE_DEVICE_GLASSES_LEFT;
  }
  
  if (gSettings.bleGlassesRightMAC.length() > 0) {
    String rightMAC = gSettings.bleGlassesRightMAC;
    rightMAC.toUpperCase();
    if (macStr == rightMAC) return BLE_DEVICE_GLASSES_RIGHT;
  }
  
  if (gSettings.bleRingMAC.length() > 0) {
    String ringMAC = gSettings.bleRingMAC;
    ringMAC.toUpperCase();
    if (macStr == ringMAC) return BLE_DEVICE_RING;
  }
  
  if (gSettings.blePhoneMAC.length() > 0) {
    String phoneMAC = gSettings.blePhoneMAC;
    phoneMAC.toUpperCase();
    if (macStr == phoneMAC) return BLE_DEVICE_PHONE;
  }
  
  return BLE_DEVICE_UNKNOWN;
}

// =============================================================================
// BLE SERVER CALLBACKS
// =============================================================================

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
    if (!gBLEState) return;
    
    // Find free connection slot
    int slot = -1;
    for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
      if (!gBLEState->connections[i].active) {
        slot = i;
        break;
      }
    }
    
    if (slot == -1) {
      BLE_DEBUGF(DEBUG_BLE_CORE, "Connection rejected - max connections reached (%d)", BLE_MAX_CONNECTIONS);
      return;
    }
    
    // Store connection info
    gBLEState->connections[slot].active = true;
    gBLEState->connections[slot].connId = param->connect.conn_id;
    gBLEState->connections[slot].connectedSince = millis();
    memcpy(gBLEState->connections[slot].deviceAddr, param->connect.remote_bda, 6);
    gBLEState->connections[slot].commandsReceived = 0;
    
    // Identify device type by MAC address
    gBLEState->connections[slot].deviceType = bleIdentifyDeviceByMAC(param->connect.remote_bda);
    gBLEState->connections[slot].deviceName = bleDeviceTypeToString(gBLEState->connections[slot].deviceType);
    
    gBLEState->activeConnectionCount++;
    gBLEState->totalConnections++;
    gBLEState->connectionState = BLE_STATE_CONNECTED;
    
    BLE_DEBUGF(DEBUG_BLE_CORE, "Client connected (slot %d, total active: %d/%d)", 
               slot, gBLEState->activeConnectionCount, BLE_MAX_CONNECTIONS);
    
    // Keep advertising if we haven't reached max connections
    if (gBLEState->activeConnectionCount >= BLE_MAX_CONNECTIONS) {
      BLE_DEBUGF(DEBUG_BLE_CORE, "Max connections reached - stopping advertising");
      BLEDevice::stopAdvertising();
    } else {
      BLE_DEBUGF(DEBUG_BLE_CORE, "Continuing advertising for additional connections");
    }
  }
  
  void onDisconnect(BLEServer* pServer) override {
    if (!gBLEState) return;
    
    // Find and clear disconnected slot
    // Note: We don't have conn_id in this callback, so we'll clear on next connect
    // This is a limitation of the current BLE library callback signature
    
    if (gBLEState->activeConnectionCount > 0) {
      gBLEState->activeConnectionCount--;
    }
    
    BLE_DEBUGF(DEBUG_BLE_CORE, "Client disconnected (active connections: %d)", 
               gBLEState->activeConnectionCount);
    
    if (gBLEState->activeConnectionCount == 0) {
      gBLEState->connectionState = BLE_STATE_IDLE;
    }
    
    // Auto-restart advertising if we're below max connections
    if (gBLEState->activeConnectionCount < BLE_MAX_CONNECTIONS && gBLEState->initialized) {
      BLE_DEBUGF(DEBUG_BLE_CORE, "Auto-restarting advertising (slots available)");
      startBLEAdvertising();
    }
  }
};

// =============================================================================
// CHARACTERISTIC CALLBACKS
// =============================================================================

// Command Request Characteristic - receives commands from client
class CmdRequestCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String value = pCharacteristic->getValue();
    if (value.length() > 0) {
      gBLEState->commandsReceived++;
      BLE_DEBUGF(DEBUG_BLE_GATT, "Command received (%d bytes)", value.length());
      processIncomingBLECommand(value.c_str(), value.length());
    }
  }
};

// Status Characteristic - returns connection status
class CmdStatusCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* pCharacteristic) override {
    char statusBuf[128];
    snprintf(statusBuf, sizeof(statusBuf),
             "{\"state\":\"connected\",\"uptime\":%lu,\"rx\":%lu,\"tx\":%lu}",
             millis() / 1000,
             gBLEState->commandsReceived,
             gBLEState->responsesSent);
    pCharacteristic->setValue(statusBuf);
    BLE_DEBUGF(DEBUG_BLE_GATT, "Status read: %s", statusBuf);
  }
};

// =============================================================================
// COMMAND PROCESSING
// =============================================================================

// External command execution function (from command_system.cpp)
extern String executeCommandThroughRegistry(const String& cmd);

// Forward declaration for OLED message history
#if ENABLE_OLED_DISPLAY
void bleAddMessageToHistory(const char* msg);
#endif

static void processIncomingBLECommand(const char* data, size_t len) {
  // Build printable command string (filter non-printable bytes, trim)
  String cmd;
  cmd.reserve(len);
  for (size_t i = 0; i < len; i++) {
    uint8_t b = ((const uint8_t*)data)[i];
    if (b == 0) continue;  // Skip NUL bytes
    if (b == '\r' || b == '\n' || b == '\t') {
      cmd += ' ';
      continue;
    }
    if (b >= 32 && b <= 126) {  // Printable ASCII only
      cmd += (char)b;
    }
  }
  cmd.trim();
  
  // Ignore empty commands
  if (cmd.length() == 0) {
    BLE_DEBUGF(DEBUG_BLE_DATA, "Ignoring empty/non-printable command");
    return;
  }
  
  // Copy to cmdBuf for logging and OLED history
  char cmdBuf[512];
  strncpy(cmdBuf, cmd.c_str(), sizeof(cmdBuf) - 1);
  cmdBuf[sizeof(cmdBuf) - 1] = '\0';
  
  BLE_DEBUGF(DEBUG_BLE_DATA, "Processing command: %s", cmdBuf);
  
  // Add to OLED message history
  #if ENABLE_OLED_DISPLAY
  bleAddMessageToHistory(cmdBuf);
  #endif
  
  // Execute through existing command registry
  String result = executeCommandThroughRegistry(String(cmdBuf));
  
  // Send response back via BLE
  if (result.length() > 0) {
    sendBLEResponse(result.c_str(), result.length());
  } else {
    // Send acknowledgment if no result
    char ack[64];
    char cmdAck[32];  // Limit command string length to 32 bytes
    strncpy(cmdAck, cmdBuf, sizeof(cmdAck) - 1);
    cmdAck[sizeof(cmdAck) - 1] = '\0';
    snprintf(ack, sizeof(ack), "{\"cmd\":\"%s\",\"status\":\"ok\"}", cmdAck);
    sendBLEResponse(ack, strlen(ack));
  }
}

// =============================================================================
// INITIALIZATION
// =============================================================================

bool initBluetooth() {
  if (gBLEState && gBLEState->initialized) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Already initialized");
    return true;
  }
  
  // Allocate state structure
  gBLEState = (BLESystemState*)ps_alloc(sizeof(BLESystemState), AllocPref::PreferPSRAM, "ble.state");
  if (!gBLEState) {
    broadcastOutput("[BLE] Failed to allocate state");
    return false;
  }
  memset(gBLEState, 0, sizeof(BLESystemState));
  
  // Initialize connection slots
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    gBLEState->connections[i].active = false;
    gBLEState->connections[i].connId = 0;
    gBLEState->connections[i].deviceType = BLE_DEVICE_UNKNOWN;
  }
  gBLEState->activeConnectionCount = 0;
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Initializing ESP32 BLE...");
  
  // Initialize ESP32 BLE with configured device name
  const char* deviceName = gSettings.bleDeviceName.length() > 0 ? gSettings.bleDeviceName.c_str() : "HardwareOne";
  BLEDevice::init(deviceName);
  
  // Set TX power level (ESP_PWR_LVL_N12 to ESP_PWR_LVL_P9)
  // Map 0-7 to actual power levels
  esp_power_level_t powerLevel = (esp_power_level_t)constrain(gSettings.bleTxPower, 0, 7);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, powerLevel);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, powerLevel);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, powerLevel);
  
  // Create server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // --------------------------------------------
  // Device Info Service (standard 0x180A)
  // --------------------------------------------
  pDeviceInfoService = pServer->createService(BLE_DEVICE_INFO_SERVICE_UUID);
  
  pManufacturerChar = pDeviceInfoService->createCharacteristic(
    BLE_MANUFACTURER_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pManufacturerChar->setValue("HardwareOne");
  
  pModelChar = pDeviceInfoService->createCharacteristic(
    BLE_MODEL_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pModelChar->setValue("ESP32-S3 Hub");
  
  pFirmwareChar = pDeviceInfoService->createCharacteristic(
    BLE_FIRMWARE_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pFirmwareChar->setValue("2.1.0");
  
  pDeviceInfoService->start();
  
  // --------------------------------------------
  // Command Service (single service for all communication)
  // --------------------------------------------
  pCommandService = pServer->createService(BLE_COMMAND_SERVICE_UUID);
  
  // Request characteristic (write from client - any command)
  pCmdRequestChar = pCommandService->createCharacteristic(
    BLE_CMD_REQUEST_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCmdRequestChar->setCallbacks(new CmdRequestCallbacks());
  
  // Response characteristic (notify to client - command results)
  pCmdResponseChar = pCommandService->createCharacteristic(
    BLE_CMD_RESPONSE_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCmdResponseChar->addDescriptor(new BLE2902());  // Required for notifications
  
  // Status characteristic (read - connection info)
  pCmdStatusChar = pCommandService->createCharacteristic(
    BLE_CMD_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  pCmdStatusChar->setCallbacks(new CmdStatusCallbacks());
  
  pCommandService->start();
  
  // --------------------------------------------
  // Data Streaming Service
  // --------------------------------------------
  pDataService = pServer->createService(BLE_DATA_SERVICE_UUID);
  
  // Sensor data characteristic (notify - continuous sensor updates)
  pSensorDataChar = pDataService->createCharacteristic(
    BLE_SENSOR_DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pSensorDataChar->addDescriptor(new BLE2902());
  
  // System status characteristic (notify - system health updates)
  pSystemStatusChar = pDataService->createCharacteristic(
    BLE_SYSTEM_STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pSystemStatusChar->addDescriptor(new BLE2902());
  
  // Event notification characteristic (notify - important events)
  pEventNotifyChar = pDataService->createCharacteristic(
    BLE_EVENT_NOTIFY_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pEventNotifyChar->addDescriptor(new BLE2902());
  
  // Stream control characteristic (write - enable/disable streams)
  pStreamControlChar = pDataService->createCharacteristic(
    BLE_STREAM_CONTROL_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  
  pDataService->start();
  
  // --------------------------------------------
  // Setup Advertising
  // --------------------------------------------
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_COMMAND_SERVICE_UUID);
  pAdvertising->addServiceUUID(BLE_DATA_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Help with iPhone connection issues
  pAdvertising->setMinPreferred(0x12);
  
  // Initialize streaming state
  gBLEState->streamFlags = BLE_STREAM_NONE;
  gBLEState->sensorStreamInterval = 1000;  // Default 1 second
  gBLEState->systemStreamInterval = 5000;  // Default 5 seconds
  gBLEState->lastSensorStream = 0;
  gBLEState->lastSystemStream = 0;
  gBLEState->sensorStreamCount = 0;
  gBLEState->systemStreamCount = 0;
  gBLEState->eventCount = 0;
  
  gBLEState->initialized = true;
  gBLEState->connectionState = BLE_STATE_IDLE;
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Bluetooth initialized successfully");
  broadcastOutput("[BLE] Initialized - ready to advertise");
  
  return true;
}

void deinitBluetooth() {
  if (!gBLEState || !gBLEState->initialized) return;
  
  BLE_DEBUGF(DEBUG_BLE_CORE, "Deinitializing Bluetooth...");
  
  stopBLEAdvertising();
  
  // ESP32 BLE doesn't have a clean disconnect API like NimBLE
  // Just deinit the device
  BLEDevice::deinit(true);
  
  // Clear all BLE object pointers (they're invalid after deinit)
  pServer = nullptr;
  pAdvertising = nullptr;
  pCommandService = nullptr;
  pDataService = nullptr;
  pDeviceInfoService = nullptr;
  pCmdRequestChar = nullptr;
  pCmdResponseChar = nullptr;
  pCmdStatusChar = nullptr;
  pSensorDataChar = nullptr;
  pSystemStatusChar = nullptr;
  pEventNotifyChar = nullptr;
  pStreamControlChar = nullptr;
  pManufacturerChar = nullptr;
  pModelChar = nullptr;
  pFirmwareChar = nullptr;
  
  // Free and clear state
  if (gBLEState) {
    free(gBLEState);
    gBLEState = nullptr;
  }
  
  broadcastOutput("[BLE] Deinitialized");
}

// =============================================================================
// ADVERTISING CONTROL
// =============================================================================

bool startBLEAdvertising() {
  if (!gBLEState || !gBLEState->initialized) {
    broadcastOutput("[BLE] Not initialized");
    return false;
  }
  
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Already connected, not advertising");
    return false;
  }
  
  if (pAdvertising) {
    BLEDevice::startAdvertising();
    gBLEState->connectionState = BLE_STATE_ADVERTISING;
    BLE_DEBUGF(DEBUG_BLE_CORE, "Advertising started");
    broadcastOutput("[BLE] Advertising started - device visible as 'HardwareOne'");
    return true;
  }
  
  return false;
}

void stopBLEAdvertising() {
  if (!gBLEState || !gBLEState->initialized) return;
  
  if (pAdvertising && gBLEState->connectionState == BLE_STATE_ADVERTISING) {
    BLEDevice::stopAdvertising();
    gBLEState->connectionState = BLE_STATE_IDLE;
    BLE_DEBUGF(DEBUG_BLE_CORE, "Advertising stopped");
  }
}

// =============================================================================
// CONNECTION MANAGEMENT
// =============================================================================

bool isBLEConnected() {
  return gBLEState && gBLEState->connectionState == BLE_STATE_CONNECTED;
}

void disconnectBLE() {
  if (!gBLEState || !pServer) return;
  
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    BLE_DEBUGF(DEBUG_BLE_CORE, "Disconnecting client...");
    // ESP32 BLE uses disconnect() with connection ID (0 for first client)
    pServer->disconnect(0);
  }
}

uint32_t getBLEConnectionDuration() {
  if (!gBLEState || gBLEState->connectionState != BLE_STATE_CONNECTED) {
    return 0;
  }
  // Return duration of first active connection
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBLEState->connections[i].active) {
      return millis() - gBLEState->connections[i].connectedSince;
    }
  }
  return 0;
}

// =============================================================================
// DATA TRANSMISSION
// =============================================================================

bool sendBLEResponse(const char* data, size_t len) {
  if (!isBLEConnected() || !pCmdResponseChar) {
    return false;
  }
  
  pCmdResponseChar->setValue((uint8_t*)data, len);
  pCmdResponseChar->notify();  // ESP32 BLE notify() works the same
  gBLEState->responsesSent++;
  
  BLE_DEBUGF(DEBUG_BLE_DATA, "Response sent (%d bytes)", len);
  return true;
}

// =============================================================================
// STATUS
// =============================================================================

bool isBLERunning() {
  return gBLEState && gBLEState->initialized;
}

const char* getBLEStateString() {
  if (!gBLEState || !gBLEState->initialized) return "uninitialized";
  
  switch (gBLEState->connectionState) {
    case BLE_STATE_IDLE:        return "idle";
    case BLE_STATE_ADVERTISING: return "advertising";
    case BLE_STATE_CONNECTED:   return "connected";
    case BLE_STATE_DISCONNECTING: return "disconnecting";
    default: return "unknown";
  }
}

void getBLEStatus(char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize < 64) return;
  
  if (!gBLEState || !gBLEState->initialized) {
    snprintf(buffer, bufferSize, "Bluetooth: disabled");
    return;
  }
  
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    snprintf(buffer, bufferSize, 
             "BLE: %d/%d connected (rx:%lu tx:%lu)",
             gBLEState->activeConnectionCount,
             BLE_MAX_CONNECTIONS,
             gBLEState->commandsReceived,
             gBLEState->responsesSent);
  } else {
    snprintf(buffer, bufferSize, 
             "BLE: %s (total: %lu)",
             getBLEStateString(),
             gBLEState->totalConnections);
  }
}

void bleApplySettings() {
  // Apply settings from gSettings if needed
  BLE_DEBUGF(DEBUG_BLE_CORE, "Settings applied");
}

// =============================================================================
// COMMAND HANDLERS
// =============================================================================

static const char* cmd_blestart(const String& cmd) {
  if (!initBluetooth()) {
    return "Failed to initialize Bluetooth";
  }
  if (!startBLEAdvertising()) {
    return "Failed to start advertising";
  }
  return "Bluetooth started and advertising";
}

static const char* cmd_blestop(const String& cmd) {
  deinitBluetooth();
  return "Bluetooth stopped";
}

static const char* cmd_blestatus(const String& cmd) {
  if (!gBLEState || !gBLEState->initialized) {
    return "Bluetooth not initialized. Run 'blestart' first.";
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  char* buf = getDebugBuffer();
  snprintf(buf, 1024, "BLE Status: %s", getBLEStateString());
  broadcastOutput(buf);
  
  snprintf(buf, 1024, "  Active connections: %d/%d", gBLEState->activeConnectionCount, BLE_MAX_CONNECTIONS);
  broadcastOutput(buf);
  
  // Show each active connection
  for (int i = 0; i < BLE_MAX_CONNECTIONS; i++) {
    if (gBLEState->connections[i].active) {
      uint32_t duration = (millis() - gBLEState->connections[i].connectedSince) / 1000;
      String macStr = macToString(gBLEState->connections[i].deviceAddr);
      snprintf(buf, 1024, "  [%d] %s", i, gBLEState->connections[i].deviceName.c_str());
      broadcastOutput(buf);
      snprintf(buf, 1024, "      MAC: %s | %lu sec | %lu cmds", 
               macStr.c_str(),
               duration,
               gBLEState->connections[i].commandsReceived);
      broadcastOutput(buf);
    }
  }
  
  snprintf(buf, 1024, "  Total connections: %lu", gBLEState->totalConnections);
  broadcastOutput(buf);
  snprintf(buf, 1024, "  Commands received: %lu", gBLEState->commandsReceived);
  broadcastOutput(buf);
  snprintf(buf, 1024, "  Responses sent: %lu", gBLEState->responsesSent);
  broadcastOutput(buf);
  
  return "OK";
}

static const char* cmd_bledisconnect(const String& cmd) {
  if (!isBLEConnected()) {
    return "No client connected";
  }
  disconnectBLE();
  return "Disconnecting client...";
}

static const char* cmd_bleadv(const String& cmd) {
  if (startBLEAdvertising()) {
    return "Advertising started";
  }
  return "Failed to start advertising";
}

static const char* cmd_blesend(const String& cmd) {
  if (!isBLEConnected()) {
    return "No client connected";
  }
  
  // Extract message after command
  String message = cmd;
  message.trim();
  
  // Remove "blesend" prefix
  int spaceIdx = message.indexOf(' ');
  if (spaceIdx > 0) {
    message = message.substring(spaceIdx + 1);
    message.trim();
  } else {
    return "Usage: blesend <message>";
  }
  
  if (message.length() == 0) {
    return "Usage: blesend <message>";
  }
  
  if (sendBLEResponse(message.c_str(), message.length())) {
    return "Message sent via BLE";
  }
  return "Failed to send message";
}

static const char* cmd_blestream(const String& cmd) {
  if (!gBLEState || !gBLEState->initialized) {
    return "Bluetooth not initialized";
  }
  
  String args = cmd;
  args.trim();
  int spaceIdx = args.indexOf(' ');
  if (spaceIdx > 0) {
    args = args.substring(spaceIdx + 1);
    args.trim();
  } else {
    // Show current status
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "Streaming: sensors=%s system=%s events=%s",
             bleIsStreamEnabled(BLE_STREAM_SENSORS) ? "ON" : "OFF",
             bleIsStreamEnabled(BLE_STREAM_SYSTEM) ? "ON" : "OFF",
             bleIsStreamEnabled(BLE_STREAM_EVENTS) ? "ON" : "OFF");
    broadcastOutput(buf);
    snprintf(buf, 1024, "Intervals: sensor=%lums system=%lums",
             (unsigned long)gBLEState->sensorStreamInterval,
             (unsigned long)gBLEState->systemStreamInterval);
    broadcastOutput(buf);
    snprintf(buf, 1024, "Stats: sensors=%lu system=%lu events=%lu",
             (unsigned long)gBLEState->sensorStreamCount,
             (unsigned long)gBLEState->systemStreamCount,
             (unsigned long)gBLEState->eventCount);
    return buf;
  }
  
  // Parse command: blestream <on|off|sensors|system|events|interval>
  if (args.startsWith("on")) {
    bleEnableStream(BLE_STREAM_ALL);
    return "All streams enabled";
  } else if (args.startsWith("off")) {
    bleDisableStream(BLE_STREAM_ALL);
    return "All streams disabled";
  } else if (args.startsWith("sensors")) {
    if (args.indexOf("off") > 0) {
      bleDisableStream(BLE_STREAM_SENSORS);
      return "Sensor stream disabled";
    } else {
      bleEnableStream(BLE_STREAM_SENSORS);
      return "Sensor stream enabled";
    }
  } else if (args.startsWith("system")) {
    if (args.indexOf("off") > 0) {
      bleDisableStream(BLE_STREAM_SYSTEM);
      return "System stream disabled";
    } else {
      bleEnableStream(BLE_STREAM_SYSTEM);
      return "System stream enabled";
    }
  } else if (args.startsWith("events")) {
    if (args.indexOf("off") > 0) {
      bleDisableStream(BLE_STREAM_EVENTS);
      return "Event stream disabled";
    } else {
      bleEnableStream(BLE_STREAM_EVENTS);
      return "Event stream enabled";
    }
  } else if (args.startsWith("interval")) {
    // Parse: blestream interval <sensor_ms> <system_ms>
    int space1 = args.indexOf(' ');
    int space2 = args.indexOf(' ', space1 + 1);
    if (space1 > 0 && space2 > 0) {
      uint32_t sensorMs = args.substring(space1 + 1, space2).toInt();
      uint32_t systemMs = args.substring(space2 + 1).toInt();
      if (sensorMs >= 100 && systemMs >= 100) {
        bleSetStreamInterval(sensorMs, systemMs);
        if (!ensureDebugBuffer()) return "Intervals set";
        char* buf = getDebugBuffer();
        snprintf(buf, 1024, "Intervals set: sensor=%lums system=%lums",
                 (unsigned long)sensorMs, (unsigned long)systemMs);
        return buf;
      }
      return "Intervals must be >= 100ms";
    }
    return "Usage: blestream interval <sensor_ms> <system_ms>";
  }
  
  return "Usage: blestream <on|off|sensors|system|events|interval>";
}

static const char* cmd_bleevent(const String& cmd) {
  if (!isBLEConnected()) {
    return "No client connected";
  }
  
  // Extract message after command
  String message = cmd;
  message.trim();
  int spaceIdx = message.indexOf(' ');
  if (spaceIdx > 0) {
    message = message.substring(spaceIdx + 1);
    message.trim();
  } else {
    return "Usage: bleevent <message>";
  }
  
  if (message.length() == 0) {
    return "Usage: bleevent <message>";
  }
  
  if (blePushEvent(BLE_EVENT_CUSTOM, message.c_str())) {
    return "Event sent via BLE";
  }
  return "Failed to send event";
}

static const char* cmd_blename(const String& cmd) {
  String args = cmd;
  args.trim();
  int spaceIdx = args.indexOf(' ');
  
  if (spaceIdx > 0) {
    String newName = args.substring(spaceIdx + 1);
    newName.trim();
    
    if (newName.length() == 0 || newName.length() > 29) {
      return "Name must be 1-29 characters";
    }
    
    gSettings.bleDeviceName = newName;
    extern bool writeSettingsJson();
    writeSettingsJson();
    
    if (!ensureDebugBuffer()) return "Name saved (restart BLE to apply)";
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "BLE name set to '%s'. Restart Bluetooth to apply (blestop && blestart)", newName.c_str());
    return buf;
  }
  
  // Show current name
  if (!ensureDebugBuffer()) return gSettings.bleDeviceName.c_str();
  char* buf = getDebugBuffer();
  snprintf(buf, 1024, "BLE Device Name: %s", gSettings.bleDeviceName.c_str());
  return buf;
}

static const char* cmd_bletxpower(const String& cmd) {
  String args = cmd;
  args.trim();
  int spaceIdx = args.indexOf(' ');
  
  if (spaceIdx > 0) {
    String levelStr = args.substring(spaceIdx + 1);
    levelStr.trim();
    int level = levelStr.toInt();
    
    if (level < 0 || level > 7) {
      return "TX power must be 0-7 (0=min/-12dBm, 7=max/+9dBm)";
    }
    
    gSettings.bleTxPower = level;
    
    // Apply immediately if BLE is running
    if (gBLEState && gBLEState->initialized) {
      esp_power_level_t powerLevel = (esp_power_level_t)level;
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, powerLevel);
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, powerLevel);
      esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, powerLevel);
    }
    
    extern bool writeSettingsJson();
    writeSettingsJson();
    
    if (!ensureDebugBuffer()) return "TX power updated";
    char* buf = getDebugBuffer();
    snprintf(buf, 1024, "BLE TX power set to level %d", level);
    return buf;
  }
  
  // Show current power level
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  snprintf(buf, 1024, "BLE TX Power: %d (0=min/-12dBm, 7=max/+9dBm)", gSettings.bleTxPower);
  return buf;
}

static const char* cmd_bleinfo(const String& cmd) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();
  
  broadcastOutput("=== BLE Configuration ===");
  snprintf(buf, 1024, "Device Name: %s", gSettings.bleDeviceName.c_str());
  broadcastOutput(buf);
  snprintf(buf, 1024, "TX Power: %d (0=min, 7=max)", gSettings.bleTxPower);
  broadcastOutput(buf);
  snprintf(buf, 1024, "Auto-Start: %s", gSettings.bluetoothAutoStart ? "Yes" : "No");
  broadcastOutput(buf);
  snprintf(buf, 1024, "Require Auth: %s", gSettings.bluetoothRequireAuth ? "Yes" : "No");
  broadcastOutput(buf);
  
  if (gBLEState && gBLEState->initialized) {
    snprintf(buf, 1024, "Status: %s", getBLEStateString());
    broadcastOutput(buf);
    snprintf(buf, 1024, "Connections: %d/%d", gBLEState->activeConnectionCount, BLE_MAX_CONNECTIONS);
    broadcastOutput(buf);
  } else {
    broadcastOutput("Status: Not initialized");
  }
  
  return "OK";
}

// =============================================================================
// COMMAND REGISTRY
// =============================================================================

const CommandEntry bluetoothCommands[] = {
  { "blestart",     "Initialize Bluetooth and start advertising",  false, cmd_blestart },
  { "blestop",      "Stop Bluetooth and deinitialize",             false, cmd_blestop },
  { "blestatus",    "Show Bluetooth connection status",            false, cmd_blestatus },
  { "bleinfo",      "Show BLE configuration and settings",         false, cmd_bleinfo },
  { "blename",      "Get/set BLE device name: blename [name]",     false, cmd_blename },
  { "bletxpower",   "Get/set BLE TX power 0-7: bletxpower [level]",false, cmd_bletxpower },
  { "bledisconnect","Disconnect current BLE client",               false, cmd_bledisconnect },
  { "bleadv",       "Start BLE advertising",                       false, cmd_bleadv },
  { "blesend",      "Send message to connected BLE client",        false, cmd_blesend },
  { "blestream",    "Control data streaming (on|off|sensors|system|events|interval)", false, cmd_blestream },
  { "bleevent",     "Send event notification to BLE client",       false, cmd_bleevent },
};

const size_t bluetoothCommandsCount = sizeof(bluetoothCommands) / sizeof(bluetoothCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _ble_cmd_registrar(bluetoothCommands, bluetoothCommandsCount, "bluetooth");

// =============================================================================
// SETTINGS
// =============================================================================

// Settings entries for Bluetooth
const SettingEntry bluetoothSettingsEntries[] = {
  { "autoStart", SETTING_BOOL, &gSettings.bluetoothAutoStart, true, 0, nullptr, 0, 1, "Auto-start at boot", nullptr },
  { "requireAuth", SETTING_BOOL, &gSettings.bluetoothRequireAuth, true, 0, nullptr, 0, 1, "Require Authentication", nullptr },
  { "deviceName", SETTING_STRING, &gSettings.bleDeviceName, true, 0, nullptr, 0, 0, "Device Name", nullptr },
  { "txPower", SETTING_INT, &gSettings.bleTxPower, true, 3, nullptr, 0, 7, "TX Power (0-7)", nullptr },
  { "glassesLeftMAC", SETTING_STRING, &gSettings.bleGlassesLeftMAC, false, 0, nullptr, 0, 0, "Glasses Left MAC", nullptr },
  { "glassesRightMAC", SETTING_STRING, &gSettings.bleGlassesRightMAC, false, 0, nullptr, 0, 0, "Glasses Right MAC", nullptr },
  { "ringMAC", SETTING_STRING, &gSettings.bleRingMAC, false, 0, nullptr, 0, 0, "Ring MAC", nullptr },
  { "phoneMAC", SETTING_STRING, &gSettings.blePhoneMAC, false, 0, nullptr, 0, 0, "Phone MAC", nullptr }
};

const size_t bluetoothSettingsCount = sizeof(bluetoothSettingsEntries) / sizeof(bluetoothSettingsEntries[0]);

// Register Bluetooth settings module
static const SettingsModule bluetoothSettingsModule = {
  "bluetooth",
  "bluetooth",
  bluetoothSettingsEntries,
  bluetoothSettingsCount
};

// Auto-register on startup
static struct BluetoothSettingsRegistrar {
  BluetoothSettingsRegistrar() { 
    extern void registerSettingsModule(const SettingsModule* module);
    registerSettingsModule(&bluetoothSettingsModule); 
  }
} _bluetooth_settings_registrar;

// =============================================================================
// OLED DISPLAY MODE
// =============================================================================

#if ENABLE_OLED_DISPLAY

// Message history for OLED display
#define BLE_MSG_HISTORY_SIZE 4
#define BLE_MSG_MAX_LEN 32

static char bleMessageHistory[BLE_MSG_HISTORY_SIZE][BLE_MSG_MAX_LEN];
static uint8_t bleMessageCount = 0;
static uint8_t bleMessageHead = 0;

// Add message to history (called when command received)
void bleAddMessageToHistory(const char* msg) {
  // Truncate if needed
  strncpy(bleMessageHistory[bleMessageHead], msg, BLE_MSG_MAX_LEN - 1);
  bleMessageHistory[bleMessageHead][BLE_MSG_MAX_LEN - 1] = '\0';
  
  bleMessageHead = (bleMessageHead + 1) % BLE_MSG_HISTORY_SIZE;
  if (bleMessageCount < BLE_MSG_HISTORY_SIZE) bleMessageCount++;
}

// =============================================================================
// BLUETOOTH OLED MENU SYSTEM
// =============================================================================

// Menu state
static int bluetoothMenuSelection = 0;
static const int BLUETOOTH_MENU_ITEMS = 5;
bool bluetoothShowingStatus = false;

// Menu items
static const char* bluetoothMenuItems[] = {
  "Status",
  "Settings",
  "Start/Stop",
  "Advertising",
  "Disconnect"
};

void bluetoothMenuUp() {
  if (bluetoothShowingStatus) return;
  if (bluetoothMenuSelection > 0) {
    bluetoothMenuSelection--;
  } else {
    bluetoothMenuSelection = BLUETOOTH_MENU_ITEMS - 1;
  }
}

void bluetoothMenuDown() {
  if (bluetoothShowingStatus) return;
  if (bluetoothMenuSelection < BLUETOOTH_MENU_ITEMS - 1) {
    bluetoothMenuSelection++;
  } else {
    bluetoothMenuSelection = 0;
  }
}

void executeBluetoothAction() {
  if (bluetoothShowingStatus) {
    bluetoothShowingStatus = false;
    return;
  }
  
  switch (bluetoothMenuSelection) {
    case 0: // Status
      bluetoothShowingStatus = true;
      break;
      
    case 1: // Settings
      if (openSettingsEditorForModule("bluetooth")) {
        extern OLEDMode currentOLEDMode;
        currentOLEDMode = OLED_SETTINGS;
      }
      break;
      
    case 2: // Start/Stop
      if (gBLEState && gBLEState->initialized) {
        deinitBluetooth();
      } else {
        initBluetooth();
        startBLEAdvertising();
      }
      break;
      
    case 3: // Advertising
      if (gBLEState && gBLEState->initialized) {
        if (gBLEState->connectionState == BLE_STATE_ADVERTISING) {
          stopBLEAdvertising();
        } else {
          startBLEAdvertising();
        }
      }
      break;
      
    case 4: // Disconnect
      if (gBLEState && gBLEState->initialized && 
          gBLEState->connectionState == BLE_STATE_CONNECTED) {
        disconnectBLE();
      }
      break;
  }
}

void bluetoothMenuBack() {
  if (bluetoothShowingStatus) {
    bluetoothShowingStatus = false;
  }
}

// Display detailed status screen
static void displayBluetoothStatusDetail() {
  oledDisplay->println("== BLE STATUS ==");
  oledDisplay->println();
  
  if (!gBLEState || !gBLEState->initialized) {
    oledDisplay->println("BLE: Disabled");
    oledDisplay->println();
    oledDisplay->println("Select Start/Stop");
    oledDisplay->println("to enable");
    return;
  }
  
  // Show device name
  oledDisplay->print("Name: ");
  String displayName = gSettings.bleDeviceName.length() > 0 ? gSettings.bleDeviceName : "HardwareOne";
  if (displayName.length() > 12) displayName = displayName.substring(0, 11) + "~";
  oledDisplay->println(displayName);
  
  // Show state
  oledDisplay->print("State: ");
  oledDisplay->println(getBLEStateString());
  
  if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
    oledDisplay->print("Clients: ");
    oledDisplay->print(gBLEState->activeConnectionCount);
    oledDisplay->print("/");
    oledDisplay->println(BLE_MAX_CONNECTIONS);
    
    oledDisplay->print("Rx:");
    oledDisplay->print(gBLEState->commandsReceived);
    oledDisplay->print(" Tx:");
    oledDisplay->println(gBLEState->responsesSent);
  } else {
    oledDisplay->print("TX Power: ");
    oledDisplay->println(gSettings.bleTxPower);
    oledDisplay->print("Total: ");
    oledDisplay->println(gBLEState->totalConnections);
  }
}

// OLED display function for Bluetooth mode
static void displayBluetoothStatus() {
  if (!oledDisplay) return;
  
  oledDisplay->setTextSize(1);
  oledDisplay->setTextColor(SSD1306_WHITE);
  oledDisplay->setCursor(0, 0);
  
  // Show status detail screen or menu
  if (bluetoothShowingStatus) {
    displayBluetoothStatusDetail();
    return;
  }
  
  // Show title with inline status
  oledDisplay->print("BLUETOOTH ");
  if (gBLEState && gBLEState->initialized) {
    if (gBLEState->connectionState == BLE_STATE_CONNECTED) {
      oledDisplay->print("[");
      oledDisplay->print(gBLEState->activeConnectionCount);
      oledDisplay->println("]");
    } else if (gBLEState->connectionState == BLE_STATE_ADVERTISING) {
      oledDisplay->println("[ADV]");
    } else {
      oledDisplay->println("[ON]");
    }
  } else {
    oledDisplay->println("[OFF]");
  }
  
  // Draw menu items (max 5 lines for content area)
  for (int i = 0; i < BLUETOOTH_MENU_ITEMS; i++) {
    if (i == bluetoothMenuSelection) {
      oledDisplay->print("> ");
    } else {
      oledDisplay->print("  ");
    }
    oledDisplay->print(bluetoothMenuItems[i]);
    
    // Show state indicators inline
    if (i == 2) { // Start/Stop
      oledDisplay->print(gBLEState && gBLEState->initialized ? " *" : "");
    } else if (i == 3) { // Advertising
      if (gBLEState && gBLEState->connectionState == BLE_STATE_ADVERTISING) {
        oledDisplay->print(" *");
      }
    }
    oledDisplay->println();
  }
}

// Availability check for Bluetooth OLED mode
static bool bluetoothOLEDModeAvailable(String* outReason) {
  return true;  // Always show in menu
}

// Input handler for Bluetooth OLED mode - menu navigation
static bool bluetoothInputHandler(int deltaX, int deltaY, uint32_t newlyPressed) {
  // Use centralized navigation events (computed with proper debounce/auto-repeat)
  if (gNavEvents.up) {
    bluetoothMenuUp();
    return true;
  }
  if (gNavEvents.down) {
    bluetoothMenuDown();
    return true;
  }
  
  // A or X button: Execute action
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_A) || INPUT_CHECK(newlyPressed, INPUT_BUTTON_X)) {
    executeBluetoothAction();
    return true;
  }
  
  // B button: Back
  if (INPUT_CHECK(newlyPressed, INPUT_BUTTON_B)) {
    if (bluetoothShowingStatus) {
      bluetoothMenuBack();
      return true;
    }
    // Return false to let main handler exit to menu
    return false;
  }
  
  return false;
}

// Bluetooth OLED mode entry
static const OLEDModeEntry bluetoothOLEDModes[] = {
  {
    OLED_BLUETOOTH,          // mode enum
    "Bluetooth",             // menu name
    "bt_idle",               // icon name (Bluetooth glyph)
    displayBluetoothStatus,  // displayFunc
    bluetoothOLEDModeAvailable, // availFunc
    bluetoothInputHandler,   // inputFunc - X toggles BLE state
    true,                    // showInMenu
    45                       // menuOrder (near ESP-NOW)
  }
};

// Auto-register Bluetooth OLED mode
REGISTER_OLED_MODE_MODULE(bluetoothOLEDModes, sizeof(bluetoothOLEDModes) / sizeof(bluetoothOLEDModes[0]), "Bluetooth");

#endif // ENABLE_OLED_DISPLAY

// ============================================================================
// Bluetooth Streaming Extensions (merged from Optional_Bluetooth_Streaming.cpp)
// ============================================================================
// Data Pipeline and Event System - provides continuous data streaming and 
// event notifications over BLE.

// External sensor cache references
#if ENABLE_THERMAL_SENSOR
extern struct ThermalCache {
  SemaphoreHandle_t mutex;
  int16_t* thermalFrame;
  float* thermalInterpolated;
  float thermalMinTemp;
  float thermalMaxTemp;
  float thermalCenterTemp;
  int thermalHottestX;
  int thermalHottestY;
  uint32_t thermalLastUpdate;
  bool thermalDataValid;
  uint32_t thermalSeq;
} gThermalCache;
extern bool thermalEnabled;
extern bool thermalConnected;
#endif

#if ENABLE_TOF_SENSOR
extern struct TofCache {
  SemaphoreHandle_t mutex;
  struct TofObject {
    bool detected;
    int distance_mm;
    float distance_cm;
    uint8_t status;
    bool valid;
    float smoothed_distance_mm;
    float smoothed_distance_cm;
    bool hasHistory;
  } tofObjects[4];
  int tofTotalObjects;
  uint32_t tofLastUpdate;
  bool tofDataValid;
  uint32_t tofSeq;
} gTofCache;
extern bool tofEnabled;
extern bool tofConnected;
#endif

#if ENABLE_IMU_SENSOR
extern struct ImuCache {
  SemaphoreHandle_t mutex;
  float accelX, accelY, accelZ;
  float gyroX, gyroY, gyroZ;
  float heading, pitch, roll;
  uint32_t imuLastUpdate;
  bool imuDataValid;
  uint32_t imuSeq;
} gImuCache;
extern bool imuEnabled;
extern bool imuConnected;
#endif

// =============================================================================
// DATA STREAMING PIPELINE
// =============================================================================

bool blePushSensorData(const char* jsonData, size_t len) {
  if (!isBLEConnected() || !pSensorDataChar) {
    return false;
  }
  
  pSensorDataChar->setValue((uint8_t*)jsonData, len);
  pSensorDataChar->notify();
  
  if (gBLEState) {
    gBLEState->sensorStreamCount++;
  }
  
  return true;
}

bool blePushSystemStatus(const char* jsonData, size_t len) {
  if (!isBLEConnected() || !pSystemStatusChar) {
    return false;
  }
  
  pSystemStatusChar->setValue((uint8_t*)jsonData, len);
  pSystemStatusChar->notify();
  
  if (gBLEState) {
    gBLEState->systemStreamCount++;
  }
  
  return true;
}

bool blePushEvent(BLEEventType eventType, const char* message, const char* details) {
  if (!isBLEConnected() || !pEventNotifyChar) {
    return false;
  }
  
  // Build event JSON
  char eventJson[256];
  int pos = snprintf(eventJson, sizeof(eventJson),
                     "{\"type\":%d,\"msg\":\"%s\"", eventType, message);
  
  if (details && details[0] != '\0') {
    pos += snprintf(eventJson + pos, sizeof(eventJson) - pos,
                    ",\"details\":\"%s\"", details);
  }
  
  pos += snprintf(eventJson + pos, sizeof(eventJson) - pos,
                  ",\"ts\":%lu}", millis());
  
  pEventNotifyChar->setValue((uint8_t*)eventJson, pos);
  pEventNotifyChar->notify();
  
  if (gBLEState) {
    gBLEState->eventCount++;
  }
  
  return true;
}

// =============================================================================
// STREAM CONTROL
// =============================================================================

void bleEnableStream(uint8_t streamFlags) {
  if (!gBLEState) return;
  gBLEState->streamFlags |= streamFlags;
}

void bleDisableStream(uint8_t streamFlags) {
  if (!gBLEState) return;
  gBLEState->streamFlags &= ~streamFlags;
}

void bleSetStreamInterval(uint32_t sensorMs, uint32_t systemMs) {
  if (!gBLEState) return;
  gBLEState->sensorStreamInterval = sensorMs;
  gBLEState->systemStreamInterval = systemMs;
}

bool bleIsStreamEnabled(uint8_t streamFlag) {
  if (!gBLEState) return false;
  return (gBLEState->streamFlags & streamFlag) != 0;
}

// =============================================================================
// AUTO-STREAMING (Call from main loop)
// =============================================================================

static void buildSensorDataJSON(char* buf, size_t bufSize) {
  int pos = snprintf(buf, bufSize, "{\"sensors\":{");
  
  #if ENABLE_THERMAL_SENSOR
  if (thermalEnabled && thermalConnected && gThermalCache.mutex) {
    if (xSemaphoreTake(gThermalCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gThermalCache.thermalDataValid) {
        pos += snprintf(buf + pos, bufSize - pos,
                        "\"thermal\":{\"min\":%.1f,\"max\":%.1f,\"center\":%.1f,\"valid\":true},",
                        gThermalCache.thermalMinTemp,
                        gThermalCache.thermalMaxTemp,
                        gThermalCache.thermalCenterTemp);
      }
      xSemaphoreGive(gThermalCache.mutex);
    }
  }
  #endif
  
  #if ENABLE_TOF_SENSOR
  if (tofEnabled && tofConnected && gTofCache.mutex) {
    if (xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gTofCache.tofDataValid && gTofCache.tofTotalObjects > 0) {
        pos += snprintf(buf + pos, bufSize - pos,
                        "\"tof\":{\"dist_mm\":%d,\"valid\":true},",
                        gTofCache.tofObjects[0].distance_mm);
      }
      xSemaphoreGive(gTofCache.mutex);
    }
  }
  #endif
  
  #if ENABLE_IMU_SENSOR
  if (imuEnabled && imuConnected && gImuCache.mutex) {
    if (xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (gImuCache.imuDataValid) {
        pos += snprintf(buf + pos, bufSize - pos,
                        "\"imu\":{\"heading\":%.1f,\"pitch\":%.1f,\"roll\":%.1f,\"valid\":true},",
                        gImuCache.heading,
                        gImuCache.pitch,
                        gImuCache.roll);
      }
      xSemaphoreGive(gImuCache.mutex);
    }
  }
  #endif
  
  // Remove trailing comma if any sensors were added
  if (pos > 12 && buf[pos - 1] == ',') {
    pos--;
  }
  
  pos += snprintf(buf + pos, bufSize - pos, "},\"ts\":%lu}", millis());
  buf[pos] = '\0';
}

static void buildSystemStatusJSON(char* buf, size_t bufSize) {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t minHeap = ESP.getMinFreeHeap();
  uint32_t freePSRAM = ESP.getFreePsram();
  uint32_t uptime = millis() / 1000;
  
  snprintf(buf, bufSize,
           "{\"system\":{\"heap_free\":%lu,\"heap_min\":%lu,\"psram_free\":%lu,\"uptime\":%lu},\"ts\":%lu}",
           (unsigned long)freeHeap,
           (unsigned long)minHeap,
           (unsigned long)freePSRAM,
           (unsigned long)uptime,
           (unsigned long)millis());
}

void bleUpdateStreams() {
  if (!gBLEState || !isBLEConnected()) return;
  
  uint32_t now = millis();
  
  // Stream sensor data
  if (bleIsStreamEnabled(BLE_STREAM_SENSORS)) {
    if (now - gBLEState->lastSensorStream >= gBLEState->sensorStreamInterval) {
      char sensorBuf[512];
      buildSensorDataJSON(sensorBuf, sizeof(sensorBuf));
      blePushSensorData(sensorBuf, strlen(sensorBuf));
      gBLEState->lastSensorStream = now;
    }
  }
  
  // Stream system status
  if (bleIsStreamEnabled(BLE_STREAM_SYSTEM)) {
    if (now - gBLEState->lastSystemStream >= gBLEState->systemStreamInterval) {
      char systemBuf[256];
      buildSystemStatusJSON(systemBuf, sizeof(systemBuf));
      blePushSystemStatus(systemBuf, strlen(systemBuf));
      gBLEState->lastSystemStream = now;
    }
  }
}

#endif // ENABLE_BLUETOOTH
